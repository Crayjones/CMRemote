#pragma once
#include <QJsonObject>
#include <QSize>
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
    bool operator==(const RdpOptions& other) const;
    QSize initialSize(QSize panel) const;
    QJsonObject json() const;
};
struct SessionTarget {
    QString host;
    quint16 port = 3389;
    QString username, domain;
    RdpOptions options;
    QString protocol = "rdp", sshAuth = "password", sshKeyPath;
    bool viewOnly = false;
    bool sameAccount(const SessionTarget& other) const {
        if (protocol != other.protocol || host != other.host || port != other.port) return false;
        if (protocol == "vnc") return true; // VNC does not use account/domain attributes.
        return username == other.username && (protocol == "ssh" ? sshAuth == other.sshAuth : domain == other.domain);
    }
    bool operator==(const SessionTarget& other) const {
        if (!sameAccount(other)) return false;
        if (protocol == "vnc") return viewOnly == other.viewOnly;
        return protocol == "ssh" ? sshKeyPath == other.sshKeyPath : options == other.options;
    }
    QString endpoint() const { return (host.contains(':') ? "[" + host + "]" : host) + ":" + QString::number(port); }
    QJsonObject overrides() const;
};
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
