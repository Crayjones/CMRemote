#pragma once
#include <QJsonObject>
#include <QSize>
#include <QStringList>
#include <QString>
#include <optional>
namespace rm { struct RdpOptions; struct RdpTarget; struct SshTarget; struct VncTarget; struct SessionInfo; }
struct RdpOptions {
    QString resolution = "1024x768";
    bool automaticResize = true;
    quint8 colorDepth = 32;
    bool consoleSession = false, cacheBitmaps = false, wallpaper = true, themes = true, fontSmoothing = true;
    // Security/gateway values use the Rust model names (see I.rdpsecurity).
    QString securityProtocol = "auto", serverAuth = "warn";
    bool useCredSsp = true;
    quint16 idleMinutes = 0;
    QString loadBalanceInfo, gatewayUsage = "never", gatewayHost;
    quint16 gatewayPort = 443;
    QString gatewayUsername, gatewayDomain, gatewayCredentials = "same";
    QString sound = "local"; // local | remote | none
    bool clipboard = false; // share text with the remote computer from the start
    bool gatewayEnabled() const { return gatewayUsage != "never"; }
    // mRemoteNG SmartSize scales a screen-sized desktop and never resizes the session.
    bool dynamicResize() const { return automaticResize && resolution != "smart_size"; }
    bool operator==(const RdpOptions& other) const;
    // screen: the display the session opens on (SmartSize and Fullscreen use its size).
    QSize initialSize(QSize panel, QSize screen = {}) const;
    QJsonObject json() const;
};
struct SessionTarget {
    QString host;
    quint16 port = 3389;
    QString username, domain;
    RdpOptions options;
    QString protocol = "rdp", sshAuth = "password", sshKeyPath;
    bool viewOnly = false;
    bool smartSize = true; // VNC: fill the panel (SmartSAspect); false keeps the server's size (SmartSNo)
    bool sameAccount(const SessionTarget& other) const {
        if (protocol != other.protocol || host != other.host || port != other.port) return false;
        // A VNC password belongs to the host and port; the username only matters to VeNCrypt Plain.
        if (protocol == "vnc") return true;
        return username == other.username && (protocol == "ssh" ? sshAuth == other.sshAuth : domain == other.domain);
    }
    bool operator==(const SessionTarget& other) const {
        if (!sameAccount(other)) return false;
        if (protocol == "vnc") return viewOnly == other.viewOnly && smartSize == other.smartSize && username == other.username;
        return protocol == "ssh" ? sshKeyPath == other.sshKeyPath : options == other.options;
    }
    QString endpoint() const { return (host.contains(':') ? "[" + host + "]" : host) + ":" + QString::number(port); }
    QJsonObject overrides() const;
};
// Core Resolution names: fit, smart_size, fullscreen, then the fixed sizes ("WxH").
// Defined here so Qt-only builds (the UI preview) can list them without the core.
inline const QStringList& rdpResolutions() {
    static const QStringList names{"fit", "smart_size", "fullscreen", "800x600", "1024x768", "1152x864", "1280x720", "1280x800", "1280x1024",
        "1366x768", "1440x900", "1600x900", "1600x1200", "1680x1050", "1920x1080", "1920x1200", "2048x1536", "2560x1440", "2560x1600", "2560x2048", "3840x2160"};
    return names;
}
rm::RdpOptions bridgeOptions(const RdpOptions& options);
rm::RdpTarget bridgeTarget(const SessionTarget& target);
rm::SshTarget bridgeSshTarget(const SessionTarget& target);
rm::VncTarget bridgeVncTarget(const SessionTarget& target);
SessionTarget sessionTarget(const rm::SessionInfo& info);
SessionTarget sessionTarget(const QJsonObject& effective);
std::optional<SessionTarget> normalizeRdpTarget(const SessionTarget& target);
std::optional<SessionTarget> normalizeSshTarget(const SessionTarget& target);
std::optional<SessionTarget> normalizeVncTarget(const SessionTarget& target);
std::optional<SessionTarget> normalizeSessionTarget(const SessionTarget& target);
std::optional<SessionTarget> parseRdpTarget(const QString& endpoint);
std::optional<SessionTarget> parseSshTarget(const QString& endpoint);
std::optional<SessionTarget> parseVncTarget(const QString& endpoint);
