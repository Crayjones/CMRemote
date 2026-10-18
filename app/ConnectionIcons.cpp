#include "ConnectionIcons.h"
#include "ThemeIcons.h"

namespace ConnectionIcons {
const QStringList& names() {
    static const QStringList catalog{
        "Admin", "Anti Virus", "Apple", "Backup", "Build Server", "Console", "Database", "Domain Controller",
        "ESX", "Fax", "File Server", "Finance", "Firewall", "Infrastructure", "Linux", "Log", "Mail Server",
        "PowerShell", "Production", "PuTTY", "RaspberryPi", "Remote Desktop", "Router", "SSH", "SharePoint",
        "Staging", "Switch", "Tel", "Telnet", "Terminal Server", "Test Server", "Virtual Machine", "WSL",
        "Web Server", "WiFi", "Windows", "Workstation", "mRemote", "mRemoteNG"
    };
    return catalog;
}
QIcon resolve(const QString& name, bool folder) {
    // Never turn imported icon names into paths. Only exact catalog entries
    // can address a resource; unknown names remain metadata with a fallback.
    if (names().contains(name)) return QIcon(":/connection-icons/" + name + ".ico");
    return ThemeIcons::themed(QIcon(folder ? ":/original/FolderClosed_16x.png" : ":/original/Connection.ico"));
}
QIcon profile(const QJsonObject& profile) {
    return resolve(profile.value("effective").toObject().value("icon").toString(), profile.value("kind") == "folder");
}
QVariantList choices(const QString& current) {
    QVariantList options{QVariantMap{{"text", "Default"}, {"value", ""}, {"icon", resolve("")}}};
    if (!current.isEmpty() && !names().contains(current))
        options.append(QVariantMap{{"text", current + " (unavailable)"}, {"value", current}, {"icon", resolve(current)}});
    for (const auto& name : names()) options.append(QVariantMap{{"text", name}, {"value", name}, {"icon", resolve(name)}});
    return options;
}
}
