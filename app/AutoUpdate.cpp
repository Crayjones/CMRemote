#include "AutoUpdate.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThreadPool>
#include <algorithm>
#include <vector>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <signal.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <winver.h>
#endif

// Q_INIT_RESOURCE must be outside a namespace so static-library linking finds
// the generated qInitResources_update symbol.
static void initializeUpdateResources() {
    static const bool initialized = [] { Q_INIT_RESOURCE(update); return true; }();
    Q_UNUSED(initialized);
}

namespace {
constexpr qint64 MaxPackage = 512ll * 1024 * 1024;
constexpr qint64 MaxSums = 64 * 1024;
constexpr qint64 MaxRecord = 4096;
constexpr int DailyMs = 24 * 60 * 60 * 1000;
constexpr qint64 StaleInstallMs = 10 * 60 * 1000;
constexpr auto Private = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;

bool safeName(const QString& name) {
    static const QRegularExpression shape("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$");
    return shape.match(name).hasMatch() && !name.contains("..");
}
bool isSha256(const QString& text) {
    static const QRegularExpression shape("^[0-9a-f]{64}$");
    return shape.match(text).hasMatch();
}
// A short status file the helper writes (its progress or why it stopped); links are ignored.
QByteArray readSmall(const QString& path) {
    if (QFileInfo(path).isSymLink()) return {};
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(MaxRecord) : QByteArray();
}
// Small JSON records the updater and its helper own; links and oversized files are ignored.
std::optional<QJsonObject> readRecord(const QString& path) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || info.size() > MaxRecord) return std::nullopt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    const auto document = QJsonDocument::fromJson(file.read(MaxRecord));
    if (!document.isObject()) return std::nullopt;
    return document.object();
}
bool writeRecord(const QString& path, const QJsonObject& record) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
    return file.write(bytes) == bytes.size() && file.commit();
}
// Removes a regular file or a link itself, never what a link points to.
void removeEntry(const QString& path) {
    const QFileInfo info(path);
    if (info.isSymLink() || info.isFile()) QFile::remove(path);
}
// Lowercase SHA-256 of a regular file of `size` bytes (0: any size), or empty.
QString hashFile(const QString& path, qint64 size) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || (size > 0 && info.size() != size) || info.size() > MaxPackage) return {};
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
QString clip(QString text) { text.replace('\n', ' '); return text.left(400); }
bool processAlive(qint64 pid) {
#ifdef Q_OS_UNIX
    return pid > 0 && (::kill(pid_t(pid), 0) == 0 || errno == EPERM);
#else
    Q_UNUSED(pid); return false;
#endif
}
void stopProcess(qint64 pid) {
#ifdef Q_OS_UNIX
    ::kill(pid_t(pid), SIGTERM);
#else
    Q_UNUSED(pid);
#endif
}
// KiB copied so far into the newest "<target>.updating-*" folder a Windows setup is filling.
qint64 stagedKiB(const QString& target) {
    const QFileInfo installed(target);
    const auto stages = installed.dir().entryInfoList({installed.fileName() + ".updating-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Time);
    if (stages.isEmpty() || stages.first().isSymLink()) return 0;
    qint64 bytes = 0;
    QDirIterator files(stages.first().filePath(), QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) { files.next(); bytes += files.fileInfo().size(); }
    return bytes / 1024;
}
#ifdef Q_OS_WIN
// Setup programs that can stage while CMRemote stays open say so in their version resource
// (scripts/release.py); older ones install only after CMRemote has closed.
bool setupStages(const QString& path) {
    const auto file = QDir::toNativeSeparators(path).toStdWString();
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size) return false;
    std::vector<char> data(size);
    if (!::GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return false;
    struct Translation { WORD language, codepage; };
    Translation* translations = nullptr; UINT length = 0;
    if (!::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length)) return false;
    for (UINT i = 0; i < length / sizeof(Translation); ++i) {
        const auto key = QString("\\StringFileInfo\\%1%2\\CMRemoteUpdate").arg(translations[i].language, 4, 16, QChar('0')).arg(translations[i].codepage, 4, 16, QChar('0')).toStdWString();
        wchar_t* value = nullptr; UINT characters = 0;
        if (::VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &characters) && characters
            && QString::fromWCharArray(value) == "staged") return true;
    }
    return false;
}
#endif
// The setup program commit() chose; runPendingSetup() starts it once CMRemote's window is gone.
struct PendingSetup { QString package, parameters, version, updates; };
std::optional<PendingSetup>& pendingSetup() { static std::optional<PendingSetup> pending; return pending; }
// As published release names and build-info.json spell it (scripts/package.py).
QString buildArchitecture() {
#if defined(Q_PROCESSOR_ARM_64)
    return "arm64";
#elif defined(Q_OS_LINUX)
    return "x86_64";
#else
    return "x64";
#endif
}
Updates::Installation unsupported(const QString& reason) { Updates::Installation none; none.reason = reason; return none; }
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX) || defined(Q_OS_WIN)
// build-info.json from scripts/package.py: only published GitHub packages of this version update themselves.
QString releasePackageProblem(const QString& path, QJsonObject* info) {
    const auto record = readRecord(path);
    if (!record || record->value("product").toString() != "CMRemote" || record->value("update_channel").toString() != "github")
        return "This is not a published CMRemote release package.";
    if (record->value("version").toString() != Updates::currentVersion() || record->value("architecture").toString() != buildArchitecture())
        return "This package's build information does not match the running version.";
    *info = *record;
    return {};
}
#endif
}

namespace Updates {
Installation detectInstallation() {
#if defined(Q_OS_MACOS)
    // .../CMRemote.app/Contents/MacOS/RemoteManager
    QDir dir(QCoreApplication::applicationDirPath());
    if (!dir.cdUp() || !dir.cdUp() || !dir.absolutePath().endsWith(".app")) return unsupported("This copy is not an application bundle.");
    const auto bundle = QFileInfo(dir.absolutePath()).canonicalFilePath();
    QJsonObject info;
    if (const auto problem = releasePackageProblem(bundle + "/Contents/Resources/build-info.json", &info); !problem.isEmpty()) return unsupported(problem);
    const auto identifier = info.value("bundle_identifier").toString();
    if (identifier.isEmpty()) return unsupported("This package has no bundle identifier.");
    if (bundle.startsWith("/Volumes/") || bundle.contains("/AppTranslocation/"))
        return unsupported("Move CMRemote to the Applications folder so it can update itself.");
    const auto folder = QFileInfo(bundle).absolutePath();
    if (!QFileInfo(folder).isWritable()) return unsupported("CMRemote cannot replace itself in " + folder + ".");
    return {Installation::MacBundle, bundle, identifier, buildArchitecture(), {}};
#elif defined(Q_OS_LINUX)
    const auto image = qEnvironmentVariable("APPIMAGE"), mount = qEnvironmentVariable("APPDIR");
    if (image.isEmpty() || mount.isEmpty()) {
        if (!qEnvironmentVariableIsEmpty("FLATPAK_ID")) return unsupported("Flatpak installations update through Flatpak.");
        return unsupported("Only the AppImage updates itself; download other packages from GitHub.");
    }
    // The running executable must come from this AppImage's own mount.
    const auto appDir = QFileInfo(mount).canonicalFilePath(), executable = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    if (appDir.isEmpty() || !executable.startsWith(appDir + "/")) return unsupported("This process was not started from its AppImage.");
    QJsonObject info;
    if (const auto problem = releasePackageProblem(appDir + "/share/build-info.json", &info); !problem.isEmpty()) return unsupported(problem);
    const QFileInfo file(image);
    if (file.isSymLink() || !file.isFile()) return unsupported("The AppImage " + image + " is a link or is missing.");
    const auto target = file.canonicalFilePath();
    if (!QFileInfo(QFileInfo(target).absolutePath()).isWritable()) return unsupported("CMRemote cannot replace " + target + ": its folder is not writable.");
    return {Installation::AppImage, target, {}, buildArchitecture(), {}};
#elif defined(Q_OS_WIN)
    // <install folder>\RemoteManager.exe beside the Uninstall.exe the setup program writes.
    const auto folder = QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
    QJsonObject info;
    if (const auto problem = releasePackageProblem(folder + "/share/build-info.json", &info); !problem.isEmpty()) return unsupported(problem);
    const QFileInfo uninstaller(folder + "/Uninstall.exe");
    if (!uninstaller.isFile() || uninstaller.isSymLink())
        return unsupported("This portable copy cannot update itself. Install CMRemote with its setup program, or download the new version from GitHub.");
    return {Installation::WindowsSetup, QDir::toNativeSeparators(folder), {}, buildArchitecture(), {}};
#else
    return unsupported("This platform cannot update itself; download the new version from GitHub.");
#endif
}
std::optional<Asset> installerFor(const QList<Asset>& assets, const QString& version, const Installation& installation) {
    QString suffix;
    switch (installation.kind) {
    case Installation::MacBundle: suffix = "-macos-" + installation.architecture + ".dmg"; break;
    case Installation::AppImage: suffix = "-linux-" + installation.architecture + ".appimage"; break;
    case Installation::WindowsSetup: suffix = "-windows-" + installation.architecture + "-setup.exe"; break;
    case Installation::None: return std::nullopt;
    }
    const auto expected = "CMRemote-" + version + suffix;
    for (const auto& asset : assets)
        if (asset.name.compare(expected, Qt::CaseInsensitive) == 0 && safeName(asset.name) && asset.size <= MaxPackage) return asset;
    return std::nullopt;
}
QString checksumFor(const QByteArray& sums, const QString& name) {
    static const QRegularExpression line("^([0-9a-fA-F]{64})\\s+\\*?(\\S+)\\s*$");
    QString found;
    for (const auto& row : sums.split('\n')) {
        const auto match = line.match(QString::fromUtf8(row));
        if (!match.hasMatch() || match.captured(2) != name) continue;
        const auto value = match.captured(1).toLower();
        if (!found.isEmpty() && found != value) return {}; // conflicting entries
        found = value;
    }
    return found;
}
}

AutoUpdater::AutoUpdater(Options options, QObject* parent)
    : QObject(parent), options_(std::move(options)),
      installation_(options_.installation ? *options_.installation : Updates::detectInstallation()),
      network_(options_.network ? options_.network : new QNetworkAccessManager(this)) {
    daily_.setInterval(DailyMs);
    connect(&daily_, &QTimer::timeout, this, [this] { checkNow(false); });
    // After the caller connects log(): recovery reports the last helper's outcome.
    QTimer::singleShot(0, this, &AutoUpdater::recover);
}
AutoUpdater::~AutoUpdater() { cancel(); }
QString AutoUpdater::directory() const { return QDir(options_.dataDirectory).filePath("updates"); }
void AutoUpdater::fail(const QString& text) { emit log("Update: " + text); }
void AutoUpdater::failDownload(const QString& text) {
    fail(text); manual_ = false;
    // Check for Updates said this version was downloading: tell the user it stopped.
    if (std::exchange(reportDownload_, false)) emit downloadFailed(downloadVersion_, text);
}

void AutoUpdater::recover() {
    if (lock_) return;
    const auto dir = directory();
    lock_ = std::make_unique<QLockFile>(QDir(dir).filePath("updater.lock"));
    const QFileInfo info(dir);
    if (info.isSymLink() || (info.exists() && !info.isDir()) || !QDir().mkpath(dir) || !QFile::setPermissions(dir, Private)) {
        unusable_ = dir + " is not a private folder, so updates cannot be stored."; fail(unusable_); return;
    }
    // One instance owns the folder; others still check, but never download or install.
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) { unusable_ = "Another CMRemote window manages updates."; emit log("Update: another CMRemote window manages updates."); return; }
    const QFileInfo installing(QDir(dir).filePath("install.lock"));
    if (installing.exists() || installing.isSymLink()) {
        if (!installing.isSymLink() && installing.isDir() && installing.lastModified().msecsTo(QDateTime::currentDateTime()) < StaleInstallMs) {
            unusable_ = "An update is still being installed."; emit log("Update: an update is still being installed."); return;
        }
        if (installing.isDir() && !installing.isSymLink()) QDir().rmdir(installing.filePath()); else removeEntry(installing.filePath());
    }
    usable_ = true;
    if (const auto failed = readRecord(QDir(dir).filePath("failed.json"))) failedVersion_ = failed->value("version").toString();
    if (const auto skipped = readRecord(QDir(dir).filePath("skipped.json"))) skippedVersion_ = skipped->value("version").toString();
    // What the helper reported after the previous exit.
    const auto resultPath = QDir(dir).filePath("result.json"), installingPath = QDir(dir).filePath("installing.json");
    auto result = readRecord(resultPath);
    // A Windows setup program reports nothing back: the version now running says whether it installed.
    if (const auto installing = readRecord(installingPath); installing && !result) {
        const auto version = installing->value("version").toString();
        const bool installed = Updates::compareVersions(Updates::currentVersion(), version).value_or(-1) >= 0;
        result = QJsonObject{{"version", version}, {"status", installed ? "installed" : "failed"},
                             {"message", installed ? QString() : QString("Setup did not finish. Close every CMRemote window before it runs.")}};
    }
    removeEntry(installingPath);
    if (result) {
        const auto version = clip(result->value("version").toString()), message = clip(result->value("message").toString());
        if (result->value("status").toString() == "installed" && Updates::compareVersions(Updates::currentVersion(), version).value_or(-1) >= 0) {
            emit log("Update: " + (message.isEmpty() ? "CMRemote " + version + " was installed." : message));
        } else {
            emit log("Update: installing CMRemote " + version + " failed: " + (message.isEmpty() ? "no details." : message) +
                     " The previous version was kept. Use Check for Updates to try again.");
            failedVersion_ = version;
            writeRecord(QDir(dir).filePath("failed.json"), {{"schema", 1}, {"version", version}});
        }
    }
    removeEntry(resultPath);
    const auto pending = readRecord(QDir(dir).filePath("pending.json"));
    const auto pendingName = pending ? pending->value("asset").toString() : QString();
    // Sessions belong to helpers of earlier runs, which have finished; partial downloads restart.
    for (const auto& entry : QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        const auto name = entry.fileName();
        if (name.startsWith("session-") && entry.isDir() && !entry.isSymLink()) QDir(entry.filePath()).removeRecursively();
        else if (name.endsWith(".part") || name.startsWith(".result.")) removeEntry(entry.filePath());
        else if (name.startsWith("CMRemote-") && name != pendingName) removeEntry(entry.filePath());
    }
    if (!pending) { removeEntry(QDir(dir).filePath("pending.json")); return; }
    const auto version = pending->value("version").toString(), sha = pending->value("sha256").toString();
    const auto size = qint64(pending->value("size").toInteger());
    const auto expected = Updates::installerFor({{pendingName, {}, sha, size}}, version, installation_);
    const bool valid = expected && isSha256(sha) && size > 0 && version != failedVersion_ && version != skippedVersion_ &&
        Updates::compareVersions(Updates::currentVersion(), version).value_or(0) < 0;
    if (!valid) {
        if (safeName(pendingName)) removeEntry(QDir(dir).filePath(pendingName));
        removeEntry(QDir(dir).filePath("pending.json")); return;
    }
    verifyCached(QDir(dir).filePath(pendingName), version, sha, size, false);
}

void AutoUpdater::setAutomatic(bool automatic) {
    recover();
    const bool first = !scheduled_, was = automatic_;
    scheduled_ = true; automatic_ = automatic;
    if (!automatic) {
        daily_.stop();
        // An explicit request keeps going; automatic work stops.
        if (busy() && !manual_) cancel();
        if (!first && was) emit log("Update: automatic updates are off.");
        return;
    }
    if (daily_.isActive()) return;
    daily_.start();
    QTimer::singleShot(options_.firstCheckMs, this, [this] { if (automatic_ && !busy()) checkNow(false); });
    // A saved update recovered before the preference was known is offered now.
    if (ready()) emit readyToInstall(readyVersion_);
}
bool AutoUpdater::offered() const {
    return ready() && usable_ && installation_.supported() && readyVersion_ != skippedVersion_ && (automatic_ || manualVersion_ == readyVersion_);
}
bool AutoUpdater::installOnExit() const { return offered() && acceptedVersion_ == readyVersion_; }
void AutoUpdater::accept() {
    if (!offered() || acceptedVersion_ == readyVersion_) return;
    acceptedVersion_ = readyVersion_;
    emit log("Update: CMRemote " + readyVersion_ + " installs when you quit.");
}
void AutoUpdater::decline() {
    if (acceptedVersion_.isEmpty()) return;
    acceptedVersion_.clear();
    emit log("Update: CMRemote " + readyVersion_ + " is not installed when you quit.");
}
void AutoUpdater::skip() {
    if (!ready()) return;
    skippedVersion_ = readyVersion_;
    writeRecord(QDir(directory()).filePath("skipped.json"), {{"schema", 1}, {"version", skippedVersion_}});
    emit log("Update: skipping CMRemote " + skippedVersion_ + ". Check for Updates offers it again.");
    discardReady();
}
void AutoUpdater::reportCheck(bool ok, const Updates::Release& latest, const QString& error, bool installable) {
    if (!reportCheck_) return;
    reportCheck_ = false;
    emit checked(ok, latest, error, installable);
}
void AutoUpdater::checkNow(bool manual) {
    recover();
    if (manual) {
        manual_ = true; reportCheck_ = true;
        // An explicit check offers again a version that failed to install or was skipped.
        if (!failedVersion_.isEmpty()) { failedVersion_.clear(); removeEntry(QDir(directory()).filePath("failed.json")); }
        if (!skippedVersion_.isEmpty()) { skippedVersion_.clear(); removeEntry(QDir(directory()).filePath("skipped.json")); }
    } else if (!automatic_) return;
    if (check_) return; // reported when it finishes
    if (reply_ || hashing_) {
        // A download already knows its release; a saved update being verified checks afterwards.
        if (manual && latest_) { reportDownload_ = true; downloadVersion_ = latest_->version; reportCheck(true, *latest_, {}, true); }
        else if (manual) checkAfterVerify_ = true;
        return;
    }
    startCheck();
}
void AutoUpdater::startCheck() {
    emit log("Update: checking " + options_.source.toString() + " for a newer release (current " + Updates::currentVersion() + ").");
    check_ = new UpdateChecker(this, options_.source, network_);
    const auto generation = generation_;
    connect(check_, &UpdateChecker::finished, this, [this, generation](bool ok, const Updates::Release& latest, const QString& error) {
        if (generation != generation_) return;
        if (check_) check_->deleteLater();
        check_.clear();
        if (!ok) { fail("check failed: " + error); reportCheck(false, {}, error, false); manual_ = false; return; }
        finishCheck(latest);
    });
    check_->check();
}
void AutoUpdater::finishCheck(const Updates::Release& release) {
    latest_ = release;
    const auto current = Updates::currentVersion();
    const auto done = [this](const Updates::Release& r, bool installable) { reportCheck(true, r, {}, installable); manual_ = false; };
    if (Updates::compareVersions(current, release.version).value_or(0) >= 0) {
        emit log("Update: CMRemote " + current + " is the latest version (GitHub has " + release.version + ").");
        done(release, false); return;
    }
    const auto asset = usable_ ? Updates::installerFor(release.assets, release.version, installation_) : std::nullopt;
    if (!asset) {
        const auto why = !installation_.supported() ? installation_.reason
            : usable_ ? "The release has no " + installation_.architecture + " package for this kind of installation." : unusable_;
        emit log("Update: CMRemote " + release.version + " is available. " + why + " Download it from " + release.url);
        done(release, false); return;
    }
    if (!manual_ && !automatic_) return;
    if (!manual_ && release.version == skippedVersion_) {
        emit log("Update: CMRemote " + release.version + " is available; you chose to skip it. Check for Updates offers it again.");
        done(release, false); return;
    }
    if (!manual_ && release.version == failedVersion_) {
        emit log("Update: CMRemote " + release.version + " is available; it is not installed again automatically because installing it failed. Use Check for Updates to try again.");
        done(release, false); return;
    }
    if (ready() && readyVersion_ == release.version) {
        if (manual_) manualVersion_ = readyVersion_;
        done(release, true);
        emit readyToInstall(readyVersion_); return;
    }
    if (ready()) discardReady(); // superseded by a newer release
    if (manual_) { reportDownload_ = true; downloadVersion_ = release.version; }
    reportCheck(true, release, {}, true);
    if (!asset->sha256.isEmpty()) { download(*asset, release.version, asset->sha256); return; }
    // Older releases publish digests only in SHA256SUMS.txt.
    const auto sums = std::find_if(release.assets.begin(), release.assets.end(), [](const auto& a) { return a.name == "SHA256SUMS.txt"; });
    if (sums == release.assets.end()) { failDownload(asset->name + " has no published SHA-256; it was not downloaded."); return; }
    QNetworkRequest request{QUrl(sums->url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "CMRemote/" + current); request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = network_->get(request); reply_ = reply;
    const auto generation = generation_;
    connect(reply, &QNetworkReply::readyRead, this, [reply] { if (reply->bytesAvailable() > MaxSums) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, asset = *asset, version = release.version] {
        reply->deleteLater();
        if (generation != generation_) return;
        reply_.clear();
        const auto bytes = reply->read(MaxSums + 1);
        if (reply->error() != QNetworkReply::NoError || bytes.size() > MaxSums) {
            failDownload("SHA256SUMS.txt download failed: " + (bytes.size() > MaxSums ? QString("it is too large.") : reply->errorString())); return;
        }
        const auto sha = Updates::checksumFor(bytes, asset.name);
        if (sha.isEmpty()) { failDownload("SHA256SUMS.txt has no single checksum for " + asset.name + "; it was not downloaded."); return; }
        download(asset, version, sha);
    });
}
void AutoUpdater::download(const Updates::Asset& asset, const QString& version, const QString& sha256) {
    const auto target = QDir(directory()).filePath(asset.name);
    // An earlier run may have finished this download.
    const QFileInfo existing(target);
    if (existing.isFile() && !existing.isSymLink()) { verifyCached(target, version, sha256, asset.size, true); return; }
    removeEntry(target);
    const auto partialPath = target + ".part";
    removeEntry(partialPath);
    partial_ = std::make_unique<QFile>(partialPath);
    if (!partial_->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        partial_.reset(); failDownload("cannot write " + partialPath + "."); return;
    }
    partial_->setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    expectedSize_ = asset.size;
    const qint64 limit = asset.size > 0 ? asset.size : MaxPackage;
    emit log("Update: downloading CMRemote " + version + " (" + asset.name + ").");
    QNetworkRequest request{QUrl(asset.url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "CMRemote/" + Updates::currentVersion());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(120000);
    auto* reply = network_->get(request); reply_ = reply;
    const auto generation = generation_;
    auto problem = std::make_shared<QString>();
    auto received = std::make_shared<qint64>(0);
    // Every chunk, including the last buffered bytes, is size-checked, written in full and hashed.
    const auto take = [this, reply, problem, received, limit] {
        if (!partial_ || !problem->isEmpty()) return;
        const auto bytes = reply->readAll();
        if (bytes.isEmpty()) return;
        if (*received + bytes.size() > limit) *problem = "it is larger than published";
        else if (partial_->write(bytes) != bytes.size()) *problem = "it could not be written to disk";
        else { hash_->addData(bytes); *received += bytes.size(); return; }
        reply->abort();
    };
    connect(reply, &QNetworkReply::readyRead, this, [this, generation, take] { if (generation == generation_) take(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, take, problem, received, target, sha256, version, name = asset.name] {
        reply->deleteLater();
        if (generation != generation_) return;
        reply_.clear();
        if (reply->error() == QNetworkReply::NoError) take();
        const bool flushed = partial_->flush(); partial_->close();
        const auto discard = [this](const QString& text) { partial_->remove(); partial_.reset(); hash_.reset(); failDownload(text); };
        if (problem->isEmpty() && reply->error() != QNetworkReply::NoError) *problem = reply->errorString();
        if (problem->isEmpty() && !flushed) *problem = "it could not be written to disk";
        if (problem->isEmpty() && expectedSize_ > 0 && *received != expectedSize_) *problem = "it is incomplete";
        if (!problem->isEmpty()) { discard("download of " + name + " failed: " + *problem + "."); return; }
        const auto actual = QString::fromLatin1(hash_->result().toHex());
        if (actual != sha256) { discard(name + " failed verification (SHA-256 " + actual + ", expected " + sha256 + "); it was deleted."); return; }
        if (!partial_->rename(target)) { discard("cannot move the download to " + target + "."); return; }
        partial_.reset(); hash_.reset();
        markReady(target, version, sha256, *received);
    });
}
void AutoUpdater::verifyCached(const QString& path, const QString& version, const QString& sha256, qint64 size, bool fresh) {
    hashing_ = true;
    const auto generation = generation_;
    QPointer<AutoUpdater> guard(this);
    QThreadPool::globalInstance()->start([guard, generation, path, version, sha256, size, fresh] {
        const auto actual = hashFile(path, size);
        const auto actualSize = QFileInfo(path).size();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, generation, path, version, sha256, actual, actualSize, fresh] {
            if (!guard || generation != guard->generation_) return;
            guard->hashing_ = false;
            if (actual == sha256) {
                if (!fresh) emit guard->log("Update: CMRemote " + version + " was already downloaded and verified.");
                guard->markReady(path, version, sha256, actualSize);
            } else {
                removeEntry(path); removeEntry(QDir(guard->directory()).filePath("pending.json"));
                if (fresh) guard->failDownload("an earlier download of CMRemote " + version + " did not match; it was deleted. Checking again downloads it anew.");
                else guard->failDownload("the saved CMRemote " + version + " update changed on disk; it was deleted.");
            }
            // Check for Updates was chosen while the saved update was being verified.
            if (std::exchange(guard->checkAfterVerify_, false) && !guard->busy()) { guard->manual_ = true; guard->startCheck(); }
        }, Qt::QueuedConnection);
    });
}
void AutoUpdater::markReady(const QString& path, const QString& version, const QString& sha256, qint64 size) {
    const QJsonObject record{{"schema", 1}, {"version", version}, {"asset", QFileInfo(path).fileName()}, {"sha256", sha256}, {"size", size}};
    if (!writeRecord(QDir(directory()).filePath("pending.json"), record)) {
        removeEntry(path); failDownload("cannot record the downloaded update; it was deleted."); return;
    }
    readyPath_ = path; readyVersion_ = version; readySha_ = sha256; readySize_ = size;
    if (manual_) manualVersion_ = version;
    manual_ = false; reportDownload_ = false;
    emit log("Update: CMRemote " + version + " was downloaded and verified (SHA-256 " + sha256 + ").");
    emit readyToInstall(version);
}
void AutoUpdater::cancel() {
    ++generation_;
    if (check_) { check_->disconnect(this); delete check_.data(); }
    check_.clear();
    if (auto reply = reply_) { reply_.clear(); reply->disconnect(this); reply->abort(); reply->deleteLater(); }
    if (partial_) { partial_->close(); partial_->remove(); partial_.reset(); }
    hash_.reset();
    hashing_ = false; manual_ = false; reportCheck_ = false; checkAfterVerify_ = false; reportDownload_ = false;
}
void AutoUpdater::discardReady() {
    removeEntry(readyPath_); removeEntry(QDir(directory()).filePath("pending.json"));
    readyPath_.clear(); readyVersion_.clear(); readySha_.clear(); readySize_ = 0; manualVersion_.clear(); acceptedVersion_.clear();
    // A prepared helper never receives a commit, so it exits without installing.
    session_.clear();
}

QString AutoUpdater::helperScript() const {
    QString platform;
    if (installation_.kind == Updates::Installation::MacBundle) platform = ":/update/install-macos.sh";
    else if (installation_.kind == Updates::Installation::AppImage) platform = ":/update/install-appimage.sh";
    else return {};
    initializeUpdateResources();
    QFile common(":/update/common.sh"), specific(platform);
    if (!common.open(QIODevice::ReadOnly) || !specific.open(QIODevice::ReadOnly)) return {};
    return "#!/bin/sh\n" + QString::fromUtf8(common.readAll()) + QString::fromUtf8(specific.readAll());
}
void AutoUpdater::failPreparation(const QString& text) {
    preparing_ = false;
    fail("not installed: " + text);
    emit preparationFinished(false, text);
}
void AutoUpdater::prepare() {
    if (preparing_) return;
    if (prepared()) { QTimer::singleShot(0, this, [this] { emit preparationFinished(true, {}); }); return; }
    if (!installOnExit()) { QTimer::singleShot(0, this, [this] { failPreparation("No verified update is waiting."); }); return; }
    preparing_ = true;
    const auto generation = generation_;
    QPointer<AutoUpdater> guard(this);
    const auto path = readyPath_, sha = readySha_; const auto size = readySize_;
    // Rehash off the GUI thread: the package must still be exactly what was verified.
    QThreadPool::globalInstance()->start([guard, generation, path, sha, size] {
        const bool intact = hashFile(path, size) == sha;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, generation, intact] {
            if (!guard) return;
            if (generation != guard->generation_ || !guard->ready()) { guard->failPreparation("The update was canceled."); return; }
            if (!intact) {
                guard->discardReady();
                guard->failPreparation("The downloaded update changed on disk; it was deleted. Check for Updates downloads it again.");
                return;
            }
            guard->startHelper();
        }, Qt::QueuedConnection);
    });
}
bool AutoUpdater::showsInstallProgress() const {
    if (installation_.kind != Updates::Installation::WindowsSetup) return true;
#ifdef Q_OS_WIN
    return ready() && setupStages(readyPath_);
#else
    return false;
#endif
}
void AutoUpdater::startHelper() {
#ifdef Q_OS_WIN
    legacySetup_ = installation_.kind == Updates::Installation::WindowsSetup && !setupStages(readyPath_);
#else
    legacySetup_ = installation_.kind == Updates::Installation::WindowsSetup;
#endif
    if (legacySetup_) {
        // An older setup installs only once CMRemote has closed: CMRemote starts it as it exits
        // (Updates::runPendingSetup) and setup waits for it to close.
        session_ = readyPath_; preparing_ = false;
        emit log("Update: CMRemote " + readyVersion_ + " is ready to install when CMRemote closes.");
        emit preparationFinished(true, {});
        return;
    }
    const auto session = QDir(directory()).filePath("session-" + QString::number(QRandomGenerator::system()->generate64(), 16));
    const auto removeSession = [session] { QDir(session).removeRecursively(); };
    if (!QDir().mkdir(session) || !QFile::setPermissions(session, Private)) { failPreparation("Cannot create " + session + "."); return; }
    const auto pid = options_.parentPid ? options_.parentPid : QCoreApplication::applicationPid();
    const auto generation = generation_;
    emit preparationProgress(-1);
#ifdef Q_OS_UNIX
    const auto script = helperScript();
    if (script.isEmpty()) { removeSession(); failPreparation("This kind of installation cannot update itself."); return; }
    const auto scriptPath = QDir(session).filePath("install.sh");
    QSaveFile file(scriptPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(script.toUtf8()) < 0 || !file.commit() || !QFile::setPermissions(scriptPath, Private)) {
        removeSession(); failPreparation("Cannot write the update helper."); return;
    }
    QProcess helper;
    QStringList arguments{scriptPath, session, QString::number(pid), readyPath_, readySha_, readyVersion_, installation_.target, directory()};
    if (installation_.kind == Updates::Installation::MacBundle)
        arguments << installation_.identifier << (installation_.architecture == "x64" ? "x86_64" : installation_.architecture);
    helper.setProgram("/bin/sh"); helper.setArguments(arguments); helper.setWorkingDirectory(directory());
    // The helper outlives the AppImage mount: drop the package's library and plugin paths.
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto* name : {"APPIMAGE", "APPDIR", "ARGV0", "OWD", "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "OPENSSL_MODULES", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"})
        environment.remove(name);
    helper.setProcessEnvironment(environment);
    helper.setStandardOutputFile(QProcess::nullDevice()); helper.setStandardErrorFile(QProcess::nullDevice());
    qint64 helperPid = 0;
    if (!helper.startDetached(&helperPid)) { removeSession(); failPreparation("Cannot start the update helper."); return; }
    watchHelper(session, generation, [helperPid] { return processAlive(helperPid); }, [helperPid] { stopProcess(helperPid); });
#elif defined(Q_OS_WIN)
    // The new version's setup is the helper. "runas" shows the Windows permission prompt while
    // CMRemote is still open; off the GUI thread, which ShellExecuteEx blocks until it is answered.
    // NSIS: quoted /SESSION= for spaces, /D= last and unquoted.
    const auto file = QDir::toNativeSeparators(readyPath_).toStdWString(), folder = QDir::toNativeSeparators(directory()).toStdWString();
    const auto parameters = QString("/SESSION=\"%1\" /PID=%2 /D=%3").arg(QDir::toNativeSeparators(session)).arg(pid).arg(installation_.target).toStdWString();
    const auto owner = reinterpret_cast<quintptr>(::GetForegroundWindow());
    QPointer<AutoUpdater> guard(this);
    QThreadPool::globalInstance()->start([guard, generation, session, file, folder, parameters, owner] {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.hwnd = reinterpret_cast<HWND>(owner); info.lpVerb = L"runas";
        info.lpFile = file.c_str(); info.lpParameters = parameters.c_str(); info.lpDirectory = folder.c_str(); info.nShow = SW_HIDE;
        const bool started = ::ShellExecuteExW(&info) && info.hProcess;
        const DWORD code = started ? 0 : ::GetLastError();
        if (SUCCEEDED(com)) ::CoUninitialize();
        const auto process = reinterpret_cast<quintptr>(started ? info.hProcess : nullptr);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, generation, session, process, code] {
            const std::shared_ptr<void> handle(reinterpret_cast<HANDLE>(process), [](HANDLE h) { if (h) ::CloseHandle(h); });
            if (!guard) return;
            if (!process) {
                QDir(session).removeRecursively();
                guard->failPreparation(code == ERROR_CANCELLED ? QString("Administrator permission was declined, so CMRemote was not updated.")
                                                               : "Windows could not start Setup (error " + QString::number(code) + ").");
                return;
            }
            guard->watchHelper(session, generation, [handle] { return ::WaitForSingleObject(handle.get(), 0) == WAIT_TIMEOUT; },
                               [handle] { ::TerminateProcess(handle.get(), 1); });
        }, Qt::QueuedConnection);
    });
#else
    removeSession(); failPreparation("This kind of installation cannot update itself.");
#endif
}
void AutoUpdater::watchHelper(const QString& session, quint64 generation, std::function<bool()> alive, std::function<void()> stop) {
    // The helper copies the new version before CMRemote commits to closing.
    const auto removeSession = [session] { QDir(session).removeRecursively(); };
    auto* wait = new QTimer(this); wait->setInterval(50);
    auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
    auto shown = std::make_shared<int>(-1); auto ticks = std::make_shared<int>(0);
    connect(wait, &QTimer::timeout, this, [this, wait, elapsed, shown, ticks, session, generation, alive, stop, removeSession] {
        const QDir dir(session);
        const bool started = QFileInfo(dir.filePath("started")).isFile(), staged = QFileInfo(dir.filePath("ready")).isFile();
        const auto refused = QFileInfo(dir.filePath("error")).isFile() ? QString::fromUtf8(readSmall(dir.filePath("error"))).trimmed() : QString();
        const bool running = alive();
        if (started && !staged && refused.isEmpty() && running) {
            qint64 done = 0, total = 0;
            if (const auto parts = QString::fromLatin1(readSmall(dir.filePath("progress"))).split(' '); parts.size() == 2) {
                done = parts.value(0).toLongLong(); total = parts.value(1).toLongLong();
            } else if (++*ticks % 5 == 0) {
                // Windows setup reports only the total; the copy beside the installed version shows how far it is.
                total = QString::fromLatin1(readSmall(dir.filePath("total"))).trimmed().toLongLong();
                done = stagedKiB(installation_.target);
            } else total = -1;
            if (total >= 0) {
                if (const int percent = total > 0 ? int(std::clamp<qint64>(done * 100 / total, 0, 100)) : -1; percent != *shown)
                    emit preparationProgress(*shown = percent);
            }
        }
        const int limit = started ? options_.stagingTimeoutMs : options_.helperTimeoutMs;
        if (!staged && refused.isEmpty() && running && elapsed->elapsed() < limit) return;
        wait->deleteLater();
        if (generation != generation_ || !ready()) {
            if (alive()) stop();
            removeSession(); failPreparation("The update was canceled."); return;
        }
        if (!staged) {
            if (alive()) stop();
            removeSession();
            failPreparation(!refused.isEmpty() ? refused : started ? "The update helper stopped before the new version was ready." : "The update helper did not start.");
            return;
        }
        emit preparationProgress(100);
        session_ = session; preparing_ = false;
        emit log("Update: CMRemote " + readyVersion_ + " is ready to install when CMRemote closes.");
        emit preparationFinished(true, {});
    });
    wait->start();
}
bool AutoUpdater::commit(bool relaunch, QString* error) {
    const auto report = [error](const QString& text) { if (error) *error = text; return false; };
    if (!prepared() || !installOnExit()) return report("No update is prepared.");
    if (installation_.kind == Updates::Installation::WindowsSetup && legacySetup_) {
        // NSIS: /UPDATE shows only the progress page, /RELAUNCH reopens CMRemote (not elevated)
        // when it closes, /D= last and unquoted.
        const auto parameters = QString("/UPDATE") + (relaunch ? " /RELAUNCH" : "") + " /D=" + installation_.target;
        if (!writeRecord(QDir(directory()).filePath("installing.json"), {{"schema", 1}, {"version", readyVersion_}})) return report("Cannot start the installation.");
        pendingSetup() = PendingSetup{QDir::toNativeSeparators(readyPath_), parameters, readyVersion_, directory()};
        return true;
    }
    // Windows: the next start judges the result from the version it runs.
    if (installation_.kind == Updates::Installation::WindowsSetup
        && !writeRecord(QDir(directory()).filePath("installing.json"), {{"schema", 1}, {"version", readyVersion_}})) return report("Cannot start the installation.");
    QSaveFile file(QDir(session_).filePath("commit"));
    if (!file.open(QIODevice::WriteOnly) || file.write(relaunch ? "1" : "0") != 1 || !file.commit()) return report("Cannot start the installation.");
    return true;
}
void Updates::runPendingSetup() {
    auto& pending = pendingSetup();
    if (!pending) return;
    const auto setup = *std::exchange(pending, std::nullopt);
#ifdef Q_OS_WIN
    // Setup's manifest requires administrator rights; "runas" shows the Windows permission prompt.
    const auto file = setup.package.toStdWString(), parameters = setup.parameters.toStdWString(), folder = QDir::toNativeSeparators(setup.updates).toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI; info.lpVerb = L"runas";
    info.lpFile = file.c_str(); info.lpParameters = parameters.c_str(); info.lpDirectory = folder.c_str(); info.nShow = SW_SHOWNORMAL;
    if (::ShellExecuteExW(&info)) return;
    const auto code = ::GetLastError();
    const QString message = code == ERROR_CANCELLED ? QString("Administrator permission was declined, so Setup did not run.")
                                                    : "Windows could not start Setup (error " + QString::number(code) + ").";
#else
    const QString message = "This platform has no setup program.";
#endif
    removeEntry(QDir(setup.updates).filePath("installing.json"));
    writeRecord(QDir(setup.updates).filePath("result.json"), {{"schema", 1}, {"status", "failed"}, {"version", setup.version}, {"message", message}});
}
