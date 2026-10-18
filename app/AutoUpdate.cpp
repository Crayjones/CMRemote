#include "AutoUpdate.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
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
#ifdef Q_OS_UNIX
#include <cerrno>
#include <signal.h>
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
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
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
#else
    return unsupported("Windows installs updates with the setup program from GitHub.");
#endif
}
std::optional<Asset> installerFor(const QList<Asset>& assets, const QString& version, const Installation& installation) {
    QString suffix;
    switch (installation.kind) {
    case Installation::MacBundle: suffix = "-macos-" + installation.architecture + ".dmg"; break;
    case Installation::AppImage: suffix = "-linux-" + installation.architecture + ".appimage"; break;
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
    const auto resultPath = QDir(dir).filePath("result.json");
    if (const auto result = readRecord(resultPath)) {
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
    if (!first && ready()) emit readyToInstall(readyVersion_);
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
        if (manual && latest_) reportCheck(true, *latest_, {}, true);
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
    reportCheck(true, release, {}, true);
    if (!asset->sha256.isEmpty()) { download(*asset, release.version, asset->sha256); return; }
    // Older releases publish digests only in SHA256SUMS.txt.
    const auto sums = std::find_if(release.assets.begin(), release.assets.end(), [](const auto& a) { return a.name == "SHA256SUMS.txt"; });
    if (sums == release.assets.end()) { fail(asset->name + " has no published SHA-256; it was not downloaded."); manual_ = false; return; }
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
            fail("SHA256SUMS.txt download failed: " + (bytes.size() > MaxSums ? QString("it is too large.") : reply->errorString())); manual_ = false; return;
        }
        const auto sha = Updates::checksumFor(bytes, asset.name);
        if (sha.isEmpty()) { fail("SHA256SUMS.txt has no single checksum for " + asset.name + "; it was not downloaded."); manual_ = false; return; }
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
        fail("cannot write " + partialPath + "."); partial_.reset(); manual_ = false; return;
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
        const auto discard = [this] { partial_->remove(); partial_.reset(); hash_.reset(); manual_ = false; };
        if (problem->isEmpty() && reply->error() != QNetworkReply::NoError) *problem = reply->errorString();
        if (problem->isEmpty() && !flushed) *problem = "it could not be written to disk";
        if (problem->isEmpty() && expectedSize_ > 0 && *received != expectedSize_) *problem = "it is incomplete";
        if (!problem->isEmpty()) { fail("download of " + name + " failed: " + *problem + "."); discard(); return; }
        const auto actual = QString::fromLatin1(hash_->result().toHex());
        if (actual != sha256) { fail(name + " failed verification (SHA-256 " + actual + ", expected " + sha256 + "); it was deleted."); discard(); return; }
        if (!partial_->rename(target)) { fail("cannot move the download to " + target + "."); discard(); return; }
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
                guard->manual_ = false;
                if (fresh) guard->fail("an earlier download of CMRemote " + version + " did not match; it was deleted. Checking again downloads it anew.");
                else guard->fail("the saved CMRemote " + version + " update changed on disk; it was deleted.");
            }
            // Check for Updates was chosen while the saved update was being verified.
            if (std::exchange(guard->checkAfterVerify_, false) && !guard->busy()) { guard->manual_ = true; guard->startCheck(); }
        }, Qt::QueuedConnection);
    });
}
void AutoUpdater::markReady(const QString& path, const QString& version, const QString& sha256, qint64 size) {
    const QJsonObject record{{"schema", 1}, {"version", version}, {"asset", QFileInfo(path).fileName()}, {"sha256", sha256}, {"size", size}};
    if (!writeRecord(QDir(directory()).filePath("pending.json"), record)) {
        fail("cannot record the downloaded update; it was deleted."); removeEntry(path); manual_ = false; return;
    }
    readyPath_ = path; readyVersion_ = version; readySha_ = sha256; readySize_ = size;
    if (manual_) manualVersion_ = version;
    manual_ = false;
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
    hashing_ = false; manual_ = false; reportCheck_ = false; checkAfterVerify_ = false;
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
void AutoUpdater::startHelper() {
#ifdef Q_OS_UNIX
    const auto script = helperScript();
    if (script.isEmpty()) { failPreparation("This kind of installation cannot update itself."); return; }
    const auto session = QDir(directory()).filePath("session-" + QString::number(QRandomGenerator::system()->generate64(), 16));
    const auto removeSession = [session] { QDir(session).removeRecursively(); };
    if (!QDir().mkdir(session) || !QFile::setPermissions(session, Private)) { failPreparation("Cannot create " + session + "."); return; }
    const auto scriptPath = QDir(session).filePath("install.sh");
    QSaveFile file(scriptPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(script.toUtf8()) < 0 || !file.commit() || !QFile::setPermissions(scriptPath, Private)) {
        removeSession(); failPreparation("Cannot write the update helper."); return;
    }
    const auto pid = options_.parentPid ? options_.parentPid : QCoreApplication::applicationPid();
    QStringList arguments{scriptPath, session, QString::number(pid), readyPath_, readySha_, readyVersion_, installation_.target, directory()};
    if (installation_.kind == Updates::Installation::MacBundle)
        arguments << installation_.identifier << (installation_.architecture == "x64" ? "x86_64" : installation_.architecture);
    QProcess helper;
    helper.setProgram("/bin/sh"); helper.setArguments(arguments); helper.setWorkingDirectory(directory());
    // The helper outlives the AppImage mount: drop the package's library and plugin paths.
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto* name : {"APPIMAGE", "APPDIR", "ARGV0", "OWD", "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "OPENSSL_MODULES", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"})
        environment.remove(name);
    helper.setProcessEnvironment(environment);
    helper.setStandardOutputFile(QProcess::nullDevice()); helper.setStandardErrorFile(QProcess::nullDevice());
    qint64 helperPid = 0;
    if (!helper.startDetached(&helperPid)) { removeSession(); failPreparation("Cannot start the update helper."); return; }
    // The helper acknowledges before CMRemote commits to closing.
    auto* wait = new QTimer(this); wait->setInterval(25);
    auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
    const auto generation = generation_;
    connect(wait, &QTimer::timeout, this, [this, wait, elapsed, session, helperPid, generation, removeSession] {
        const bool acknowledged = QFileInfo(QDir(session).filePath("ready")).isFile();
        if (!acknowledged && processAlive(helperPid) && elapsed->elapsed() < options_.helperTimeoutMs) return;
        wait->deleteLater();
        if (generation != generation_ || !ready()) { failPreparation("The update was canceled."); return; }
        if (!acknowledged) {
            if (processAlive(helperPid)) ::kill(pid_t(helperPid), SIGTERM);
            removeSession(); failPreparation("The update helper did not start."); return;
        }
        session_ = session; preparing_ = false;
        emit log("Update: CMRemote " + readyVersion_ + " is ready to install when CMRemote closes.");
        emit preparationFinished(true, {});
    });
    wait->start();
#else
    failPreparation("This kind of installation cannot update itself.");
#endif
}
bool AutoUpdater::commit(bool relaunch, QString* error) {
    const auto report = [error](const QString& text) { if (error) *error = text; return false; };
    if (!prepared() || !installOnExit()) return report("No update is prepared.");
    QSaveFile file(QDir(session_).filePath("commit"));
    if (!file.open(QIODevice::WriteOnly) || file.write(relaunch ? "1" : "0") != 1 || !file.commit()) return report("Cannot start the installation.");
    return true;
}
