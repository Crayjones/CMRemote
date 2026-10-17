#include "RdpTarget.h"
#include "rust/cxx.h"
#include "remotemanager-core/src/bridge.rs.h"
static QString qstr(const rust::String& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
bool RdpOptions::operator==(const RdpOptions& other) const { return json() == other.json(); }
QSize RdpOptions::initialSize(QSize panel) const {
    if (resolution == "fit") return QSize(qBound(200, panel.width(), 4096), qBound(200, panel.height(), 4096));
    if (resolution == "800x600") return {800, 600};
    if (resolution == "1280x720") return {1280, 720};
    if (resolution == "1920x1080") return {1920, 1080};
    return {1024, 768};
}
QJsonObject RdpOptions::json() const {
    return {{"resolution", resolution}, {"automatic_resize", automaticResize}, {"color_depth", int(colorDepth)},
        {"console_session", consoleSession}, {"cache_bitmaps", cacheBitmaps}, {"wallpaper", wallpaper}, {"themes", themes}, {"font_smoothing", fontSmoothing},
        {"security_protocol", securityProtocol}, {"server_auth", serverAuth}, {"use_credssp", useCredSsp}, {"idle_minutes", int(idleMinutes)},
        {"load_balance_info", loadBalanceInfo}, {"gateway_usage", gatewayUsage}, {"gateway_host", gatewayHost}, {"gateway_port", int(gatewayPort)},
        {"gateway_username", gatewayUsername}, {"gateway_domain", gatewayDomain}, {"gateway_credentials", gatewayCredentials}, {"sound", sound}, {"clipboard", clipboard}};
}
QJsonObject SessionTarget::overrides() const {
    auto result = protocol == "vnc" ? QJsonObject{{"view_only", viewOnly}} :
        protocol == "ssh" ? QJsonObject{{"ssh_auth", sshAuth}, {"ssh_key_path", sshKeyPath}} : options.json();
    result.insert("protocol", protocol); result.insert("host", host); result.insert("port", int(port));
    if (protocol != "vnc") { result.insert("username", username); result.insert("domain", protocol == "ssh" ? QString() : domain); }
    return result;
}
rm::RdpOptions bridgeOptions(const RdpOptions& o) {
    const auto utf8 = [](const QString& text) { return text.toUtf8().toStdString(); };
    return {o.resolution.toStdString(), o.automaticResize, o.colorDepth, o.consoleSession, o.cacheBitmaps, o.wallpaper, o.themes, o.fontSmoothing,
        utf8(o.securityProtocol), utf8(o.serverAuth), o.useCredSsp, o.idleMinutes, utf8(o.loadBalanceInfo), utf8(o.gatewayUsage),
        utf8(o.gatewayHost), o.gatewayPort, utf8(o.gatewayUsername), utf8(o.gatewayDomain), utf8(o.gatewayCredentials), utf8(o.sound), o.clipboard};
}
rm::RdpTarget bridgeTarget(const SessionTarget& target) {
    return {target.host.toUtf8().toStdString(), target.port, target.username.toUtf8().toStdString(), target.domain.toUtf8().toStdString(), bridgeOptions(target.options)};
}
rm::SshTarget bridgeSshTarget(const SessionTarget& target) {
    return {target.host.toUtf8().toStdString(), target.port, target.username.toUtf8().toStdString(), target.sshAuth.toStdString(), target.sshKeyPath.toUtf8().toStdString()};
}
rm::VncTarget bridgeVncTarget(const SessionTarget& target) {
    return {target.host.toUtf8().toStdString(), target.port, target.viewOnly};
}
SessionTarget sessionTarget(const rm::SessionInfo& info) {
    const auto& o = info.options;
    return {qstr(info.host), info.port, qstr(info.username), qstr(info.domain),
        {qstr(o.resolution), o.automatic_resize, o.color_depth, o.console_session, o.cache_bitmaps, o.wallpaper, o.themes, o.font_smoothing,
         qstr(o.security_protocol), qstr(o.server_auth), o.use_credssp, o.idle_minutes, qstr(o.load_balance_info), qstr(o.gateway_usage),
         qstr(o.gateway_host), o.gateway_port, qstr(o.gateway_username), qstr(o.gateway_domain), qstr(o.gateway_credentials), qstr(o.sound), o.clipboard},
        qstr(info.protocol), qstr(info.ssh_auth), qstr(info.ssh_key_path), info.view_only};
}
SessionTarget sessionTarget(const QJsonObject& e) {
    return {e.value("host").toString(), quint16(e.value("port").toInt()), e.value("username").toString(), e.value("domain").toString(),
        {e.value("resolution").toString("1024x768"), e.value("automatic_resize").toBool(true), quint8(e.value("color_depth").toInt(32)),
         e.value("console_session").toBool(), e.value("cache_bitmaps").toBool(), e.value("wallpaper").toBool(true), e.value("themes").toBool(true), e.value("font_smoothing").toBool(true),
         e.value("security_protocol").toString("auto"), e.value("server_auth").toString("warn"), e.value("use_credssp").toBool(true),
         quint16(qBound(0, e.value("idle_minutes").toInt(0), 240)), e.value("load_balance_info").toString(), e.value("gateway_usage").toString("never"),
         e.value("gateway_host").toString(), quint16(e.value("gateway_port").toInt(443)), e.value("gateway_username").toString(),
         e.value("gateway_domain").toString(), e.value("gateway_credentials").toString("same"), e.value("sound").toString("local"), e.value("clipboard").toBool()},
         e.value("protocol").toString("rdp"), e.value("ssh_auth").toString("password"), e.value("ssh_key_path").toString(), e.value("view_only").toBool()};
}
std::optional<SessionTarget> normalizeSshTarget(const SessionTarget& target) {
    if (target.protocol != "ssh") return std::nullopt;
    try {
        const auto result = rm::normalize_ssh_target(bridgeSshTarget(target));
        auto normalized = target; normalized.host = qstr(result.host); normalized.domain.clear(); return normalized;
    } catch (const rust::Error&) { return std::nullopt; }
}
std::optional<SessionTarget> normalizeRdpTarget(const SessionTarget& target) {
    if (target.protocol != "rdp") return std::nullopt;
    try {
        const auto result = rm::normalize_rdp_target(bridgeTarget(target));
        auto normalized = target; normalized.host = qstr(result.host); return normalized;
    } catch (const rust::Error&) { return std::nullopt; }
}
std::optional<SessionTarget> normalizeVncTarget(const SessionTarget& target) {
    if (target.protocol != "vnc") return std::nullopt;
    try {
        const auto result = rm::normalize_vnc_target(bridgeVncTarget(target));
        auto normalized = target; normalized.host = qstr(result.host); normalized.username.clear(); normalized.domain.clear();
        return normalized;
    } catch (const rust::Error&) { return std::nullopt; }
}
std::optional<SessionTarget> normalizeSessionTarget(const SessionTarget& target) {
    if (target.protocol == "vnc") return normalizeVncTarget(target);
    return target.protocol == "ssh" ? normalizeSshTarget(target) : normalizeRdpTarget(target);
}
static std::optional<SessionTarget> parseTarget(const QString& endpoint, const QString& protocol) {
    for (const auto ch : endpoint) if (ch.category() == QChar::Other_Control) return std::nullopt;
    const auto text = endpoint.trimmed(); QString host, portText; bool explicitPort = false;
    if (text.startsWith('[')) {
        const auto end = text.indexOf(']'); if (end < 0) return std::nullopt;
        host = text.mid(1, end - 1); if (!host.contains(':')) return std::nullopt;
        const auto suffix = text.mid(end + 1);
        if (!suffix.isEmpty()) { if (!suffix.startsWith(':')) return std::nullopt; explicitPort = true; portText = suffix.mid(1); }
    } else if (text.count(':') == 1) {
        host = text.section(':', 0, 0); portText = text.section(':', 1); explicitPort = true;
    } else { host = text; }
    uint port = protocol == "vnc" ? 5900 : protocol == "ssh" ? 22 : 3389;
    if (explicitPort) {
        if (portText.isEmpty()) return std::nullopt;
        for (const auto ch : portText) if (ch < '0' || ch > '9') return std::nullopt;
        bool ok = false; port = portText.toUInt(&ok); if (!ok || port == 0 || port > 65535) return std::nullopt;
    }
    SessionTarget target{host, quint16(port), {}}; target.protocol = protocol;
    return normalizeSessionTarget(target);
}
std::optional<SessionTarget> parseRdpTarget(const QString& endpoint) { return parseTarget(endpoint, "rdp"); }
std::optional<SessionTarget> parseSshTarget(const QString& endpoint) { return parseTarget(endpoint, "ssh"); }
std::optional<SessionTarget> parseVncTarget(const QString& endpoint) { return parseTarget(endpoint, "vnc"); }
