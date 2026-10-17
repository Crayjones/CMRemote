#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <stdexcept>

inline QString applicationConnectionPath(const QString& directory) {
    return QDir(directory).filePath("confCons.xml");
}

inline QString applicationSshHostKeyPath(const QString& directory) {
    return QDir(directory).filePath("ssh-host-keys.json");
}

inline QString applicationTerminalAppearancePath(const QString& directory) {
    return QDir(directory).filePath("terminal-appearance.json");
}

// User data lives in ~/.config/CMRemote ($XDG_CONFIG_HOME honored) on macOS/Linux, %APPDATA%\CMRemote on Windows.
inline QString defaultDataDirectory() {
#ifdef Q_OS_WIN
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty() || !QDir::isAbsolutePath(base)) base = QDir::homePath() + "/AppData/Roaming";
#else
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty() || !QDir::isAbsolutePath(base)) base = QDir::homePath() + "/.config";
#endif
    return QDir::cleanPath(base + "/CMRemote");
}

// Before CMRemote 0.1.0 data used Qt's per-app location (e.g. ~/Library/Application Support/RemoteManager/RemoteManager).
inline QString legacyDataDirectory() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation); }

namespace data_paths_detail {
// Copies files that do not exist at the destination; never overwrites, skips locks and `skip`.
inline bool copyMissing(const QString& from, const QString& to, const QString& skip = {}) {
    if (!QDir().mkpath(to) || !QFile::setPermissions(to, QFileInfo(from).permissions())) return false;
    for (const auto& entry : QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        const QString target = QDir(to).filePath(entry.fileName());
        if (entry.isDir()) { if (!copyMissing(entry.filePath(), target)) return false; continue; }
        if (entry.suffix() == "lock" || entry.fileName() == skip || QFileInfo::exists(target)) continue;
        if (!QFile::copy(entry.filePath(), target) || !QFile::setPermissions(target, entry.permissions())) return false;
    }
    return true;
}
}

// Copies data from the legacy location once, when the new directory has no connection file yet.
// The legacy directory is never modified. confCons.xml is copied last, so an interrupted copy is retried.
inline bool migrateLegacyData(const QString& directory, const QString& legacy = legacyDataDirectory()) {
    const QString connections = QDir(directory).filePath("confCons.xml");
    const QString old = QDir(legacy).filePath("confCons.xml");
    if (QFileInfo::exists(connections) || legacy.isEmpty() || !QFileInfo(old).isFile()
        || QDir::cleanPath(directory) == QDir::cleanPath(legacy)) return true;
    const QString staging = connections + ".migrating";
    QFile::remove(staging);
    return data_paths_detail::copyMissing(legacy, directory, "confCons.xml")
        && QFile::copy(old, staging) && QFile::setPermissions(staging, QFileInfo(old).permissions())
        && QFile::rename(staging, connections);
}

inline QString applicationDataDirectory(const QString& developmentRoot = {}) {
    if (developmentRoot.isEmpty()) return defaultDataDirectory();
    if (!QFileInfo(developmentRoot).isAbsolute() || !QFileInfo::exists(developmentRoot + "/Cargo.toml"))
        throw std::invalid_argument("--dev-root requires an absolute RemoteManager project path");
#ifdef Q_OS_MACOS
    return QDir::cleanPath(developmentRoot + "/.dev/macos/data");
#else
    return QDir::cleanPath(developmentRoot + "/.dev/linux/data");
#endif
}
