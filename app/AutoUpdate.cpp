#include "AutoUpdate.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>

namespace {
constexpr qint64 MaxInstaller = 512ll * 1024 * 1024;
constexpr int DailyMs = 24 * 60 * 60 * 1000;
constexpr int FirstCheckMs = 20 * 1000;

#ifdef Q_OS_MACOS
QString bundlePath() {
    // .../Name.app/Contents/MacOS/RemoteManager
    QDir dir(QCoreApplication::applicationDirPath());
    if (!dir.cdUp() || !dir.cdUp()) return {};
    const auto path = dir.absolutePath();
    return path.endsWith(".app") ? path : QString();
}
const char* MacScript = R"(#!/bin/sh
# CMRemote updater: replace the application bundle after the app quits.
pid="$1"; dmg="$2"; app="$3"; relaunch="$4"
while kill -0 "$pid" 2>/dev/null; do sleep 0.5; done
mnt=$(mktemp -d "${TMPDIR:-/tmp}/cmremote-update.XXXXXX") || exit 1
if ! hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$mnt" "$dmg" >/dev/null; then rmdir "$mnt"; exit 1; fi
src=$(find "$mnt" -maxdepth 1 -name "*.app" -print | head -n 1)
status=1
if [ -n "$src" ]; then
  rm -rf "$app.updating" "$app.old"
  if ditto "$src" "$app.updating" && mv "$app" "$app.old" && mv "$app.updating" "$app"; then
    rm -rf "$app.old"; status=0
  else
    [ -d "$app" ] || mv "$app.old" "$app"
    rm -rf "$app.updating"
  fi
fi
hdiutil detach "$mnt" -quiet || hdiutil detach "$mnt" -force -quiet
rmdir "$mnt" 2>/dev/null
[ "$status" = 0 ] && rm -f "$dmg"
[ "$relaunch" = 1 ] && open "$app"
exit $status
)";
#elif defined(Q_OS_WIN)
const char* WindowsScript = "@echo off\r\n"
    "rem CMRemote updater: run the installer silently after the app quits.\r\n"
    ":wait\r\n"
    "tasklist /FI \"PID eq %1\" 2>NUL | find \"%1\" >NUL && (timeout /t 1 /nobreak >NUL & goto wait)\r\n"
    "start \"\" /wait \"%~2\" /S\r\n"
    "if \"%3\"==\"1\" start \"\" \"%~4\"\r\n"
    "del \"%~2\"\r\n";
#else
const char* AppImageScript = R"(#!/bin/sh
# CMRemote updater: replace the AppImage after the app quits.
pid="$1"; image="$2"; target="$3"; relaunch="$4"
while kill -0 "$pid" 2>/dev/null; do sleep 0.5; done
if cp "$image" "$target.updating" && chmod 755 "$target.updating" && mv -f "$target.updating" "$target"; then rm -f "$image"; else rm -f "$target.updating"; fi
[ "$relaunch" = 1 ] && nohup "$target" >/dev/null 2>&1 &
exit 0
)";
#endif
bool writeScript(const QString& path, const char* text) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit()) return false;
    return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
}

namespace Updates {
std::optional<Asset> installerFor(const QList<Asset>& assets, const QString& version) {
    QString suffix;
#if defined(Q_OS_MACOS)
#if defined(Q_PROCESSOR_ARM_64)
    suffix = "-macos-arm64.dmg";
#else
    suffix = "-macos-x64.dmg";
#endif
    if (bundlePath().isEmpty()) return std::nullopt;
#elif defined(Q_OS_WIN)
    // Only the setup installs itself; a portable copy has no uninstaller beside it.
    if (!QFileInfo::exists(QCoreApplication::applicationDirPath() + "/Uninstall.exe")) return std::nullopt;
    suffix = "-windows-x64-setup.exe";
#else
    if (qEnvironmentVariableIsEmpty("APPIMAGE")) return std::nullopt;
    suffix = "-linux-x86_64.appimage";
#endif
    const auto expected = "CMRemote-" + version + suffix;
    for (const auto& asset : assets) if (asset.name.compare(expected, Qt::CaseInsensitive) == 0) return asset;
    return std::nullopt;
}
QString checksumFor(const QByteArray& sums, const QString& name) {
    static const QRegularExpression line("^([0-9a-fA-F]{64})\\s+\\*?(\\S+)\\s*$");
    for (const auto& row : sums.split('\n')) {
        const auto match = line.match(QString::fromUtf8(row));
        if (match.hasMatch() && match.captured(2) == name) return match.captured(1).toLower();
    }
    return {};
}
}

AutoUpdater::AutoUpdater(QString dataDirectory, QObject* parent, QUrl source)
    : QObject(parent), dataDirectory_(std::move(dataDirectory)), source_(std::move(source)), network_(new QNetworkAccessManager(this)) {
    daily_.setInterval(DailyMs);
    connect(&daily_, &QTimer::timeout, this, &AutoUpdater::checkNow);
}
AutoUpdater::~AutoUpdater() {
    if (download_) download_->abort();
    if (partial_) partial_->remove();
}
QString AutoUpdater::directory() const { return QDir(dataDirectory_).filePath("updates"); }
void AutoUpdater::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    if (!enabled) { daily_.stop(); emit log("Automatic updates are off."); return; }
    daily_.start();
    QTimer::singleShot(FirstCheckMs, this, [this] { if (enabled_) checkNow(); });
}
void AutoUpdater::fail(const QString& text) { emit log("Update: " + text); }
void AutoUpdater::checkNow() {
    if (busy() || ready()) { if (ready()) emit readyToInstall(readyVersion_); return; }
    emit log("Update: checking " + source_.toString() + " for a newer release (current " + Updates::currentVersion() + ").");
    check_ = new UpdateChecker(this, source_);
    connect(check_, &UpdateChecker::finished, this, [this](bool ok, const Updates::Release& latest, const QString& error) {
        if (check_) check_->deleteLater();
        check_.clear();
        if (!ok) { fail("check failed: " + error); return; }
        finishCheck(latest);
    });
    check_->check();
}
void AutoUpdater::finishCheck(const Updates::Release& release) {
    const auto current = Updates::currentVersion();
    if (Updates::compareVersions(current, release.version).value_or(0) >= 0) {
        emit log("Update: CMRemote " + current + " is the latest version (GitHub has " + release.version + ")."); return;
    }
    const auto asset = Updates::installerFor(release.assets, release.version);
    if (!asset) {
        emit log("Update: CMRemote " + release.version + " is available, but this copy cannot install it automatically "
                 "(portable, Flatpak or development build). Download it from " + release.url); return;
    }
    if (!asset->sha256.isEmpty()) { download(*asset, release.version, asset->sha256); return; }
    // Older releases publish digests only in SHA256SUMS.txt.
    const auto sums = std::find_if(release.assets.begin(), release.assets.end(), [](const auto& a) { return a.name == "SHA256SUMS.txt"; });
    if (sums == release.assets.end()) { fail(asset->name + " has no published SHA-256; not downloading it."); return; }
    emit log("Update: fetching SHA256SUMS.txt for " + asset->name + ".");
    QNetworkRequest request{QUrl(sums->url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "CMRemote/" + current); request.setTransferTimeout(30000);
    auto* reply = network_->get(request); download_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, asset = *asset, version = release.version] {
        reply->deleteLater(); download_.clear();
        if (reply->error() != QNetworkReply::NoError) { fail("SHA256SUMS.txt download failed: " + reply->errorString()); return; }
        const auto sha = Updates::checksumFor(reply->read(65536), asset.name);
        if (sha.isEmpty()) { fail("SHA256SUMS.txt does not list " + asset.name + "; not downloading it."); return; }
        download(asset, version, sha);
    });
}
void AutoUpdater::download(const Updates::Asset& asset, const QString& version, const QString& sha256) {
    if (!QDir().mkpath(directory())) { fail("cannot create " + directory()); return; }
    const auto target = QDir(directory()).filePath(asset.name);
    // A previous run may already have downloaded and verified it.
    if (QFileInfo::exists(target)) {
        QFile existing(target); QCryptographicHash hash(QCryptographicHash::Sha256);
        if (existing.open(QIODevice::ReadOnly) && hash.addData(&existing) && hash.result().toHex() == sha256.toLatin1()) {
            readyPath_ = target; readyVersion_ = version;
            emit log("Update: CMRemote " + version + " was already downloaded and verified (" + target + ").");
            emit readyToInstall(version); return;
        }
        QFile::remove(target);
    }
    partial_ = std::make_unique<QFile>(target + ".part");
    if (!partial_->open(QIODevice::WriteOnly | QIODevice::Truncate)) { fail("cannot write " + partial_->fileName()); partial_.reset(); return; }
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    downloading_ = version;
    emit log("Update: downloading CMRemote " + version + " (" + asset.name + ") from " + asset.url);
    QNetworkRequest request{QUrl(asset.url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "CMRemote/" + Updates::currentVersion());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(120000);
    auto* reply = network_->get(request); download_ = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        const auto bytes = reply->readAll();
        if (!partial_ || partial_->size() + bytes.size() > MaxInstaller || partial_->write(bytes) != bytes.size()) { reply->abort(); return; }
        hash_->addData(bytes);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, target, sha256, version, name = asset.name] {
        reply->deleteLater(); download_.clear();
        if (!partial_) return;
        const auto rest = reply->readAll(); if (!rest.isEmpty() && partial_->write(rest) == rest.size()) hash_->addData(rest);
        partial_->close();
        const auto discard = [this] { partial_->remove(); partial_.reset(); hash_.reset(); downloading_.clear(); };
        if (reply->error() != QNetworkReply::NoError) { fail("download of " + name + " failed: " + reply->errorString()); discard(); return; }
        const auto actual = QString::fromLatin1(hash_->result().toHex());
        if (actual != sha256) { fail(name + " failed verification (SHA-256 " + actual + ", expected " + sha256 + "); it was deleted."); discard(); return; }
        if (!partial_->rename(target)) { fail("cannot move the download to " + target); discard(); return; }
        emit log("Update: CMRemote " + version + " downloaded and verified (SHA-256 " + sha256 + "). It installs when CMRemote restarts or quits.");
        partial_.reset(); hash_.reset(); downloading_.clear();
        readyPath_ = target; readyVersion_ = version;
        emit readyToInstall(version);
    });
}
bool AutoUpdater::install(bool relaunch, QString* error) {
    const auto report = [error](const QString& text) { if (error) *error = text; return false; };
    if (!ready() || !QFileInfo::exists(readyPath_)) return report("No verified update is waiting.");
    const auto pid = QString::number(QCoreApplication::applicationPid());
#if defined(Q_OS_MACOS)
    const auto bundle = bundlePath();
    if (bundle.isEmpty()) return report("This copy is not an application bundle.");
    if (!QFileInfo(QFileInfo(bundle).absolutePath()).isWritable()) return report("Cannot replace " + bundle + ": its folder is not writable.");
    const auto script = QDir(directory()).filePath("install.sh");
    if (!writeScript(script, MacScript)) return report("Cannot write the update helper.");
    if (!QProcess::startDetached("/bin/sh", {script, pid, readyPath_, bundle, relaunch ? "1" : "0"})) return report("Cannot start the update helper.");
#elif defined(Q_OS_WIN)
    const auto script = QDir(directory()).filePath("install.cmd");
    if (!writeScript(script, WindowsScript)) return report("Cannot write the update helper.");
    const auto exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (!QProcess::startDetached("cmd.exe", {"/c", QDir::toNativeSeparators(script), pid, QDir::toNativeSeparators(readyPath_), relaunch ? "1" : "0", exe}))
        return report("Cannot start the update helper.");
#else
    const auto image = qEnvironmentVariable("APPIMAGE");
    if (image.isEmpty()) return report("Only the AppImage updates itself.");
    const auto script = QDir(directory()).filePath("install.sh");
    if (!writeScript(script, AppImageScript)) return report("Cannot write the update helper.");
    if (!QProcess::startDetached("/bin/sh", {script, pid, readyPath_, image, relaunch ? "1" : "0"})) return report("Cannot start the update helper.");
#endif
    readyPath_.clear();
    return true;
}
