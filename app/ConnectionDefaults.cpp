#include "ConnectionDefaults.h"
#include <QJsonDocument>
#include <QStringList>

namespace {
QJsonObject baseline() {
    return {{"description", ""}, {"icon", ""}, {"username", ""}, {"domain", ""}, {"protocol", "rdp"}, {"port", 3389},
        {"console_session", false}, {"resolution", "1024x768"}, {"automatic_resize", true}, {"color_depth", 32},
        {"cache_bitmaps", false}, {"wallpaper", true}, {"themes", true}, {"font_smoothing", true},
        {"security_protocol", "auto"}, {"server_auth", "warn"}, {"use_credssp", true}, {"idle_minutes", 0}, {"load_balance_info", ""},
        {"gateway_usage", "never"}, {"gateway_host", ""}, {"gateway_port", 443}, {"gateway_username", ""}, {"gateway_domain", ""}, {"gateway_credentials", "same"}, {"sound", "local"}, {"clipboard", false},
        {"ssh_auth", "password"}, {"ssh_key_path", ""}, {"view_only", false}};
}
}
QJsonObject ConnectionDefaults::effective() const {
    auto result = baseline();
    for (auto it = values.begin(); it != values.end(); ++it) result[it.key()] = it.value();
    if (!values.contains("port")) result["port"] = result.value("protocol") == "ssh" ? 22 : result.value("protocol") == "vnc" ? 5900 : 3389;
    return result;
}
QJsonObject ConnectionDefaults::overrides() const {
    QJsonObject result; const auto settings = effective();
    for (auto it = settings.begin(); it != settings.end(); ++it) if (!inherits(it.key())) result[it.key()] = it.value();
    return result;
}
std::optional<ConnectionDefaults> ConnectionDefaults::parse(const QJsonObject& object) {
    if (object.size() != 2 || !object.value("values").isObject() || !object.value("inheritance").isObject() || QJsonDocument(object).toJson().size() > 32768) return {};
    ConnectionDefaults result{object.value("values").toObject(), object.value("inheritance").toObject()};
    const auto defaults = baseline();
    for (auto it = result.values.begin(); it != result.values.end(); ++it) {
        const auto key = it.key(); const auto value = it.value();
        if (!defaults.contains(key) || value.type() != defaults.value(key).type()) return {};
        if (value.isString()) {
            const auto text = value.toString(); const auto limit = key == "icon" || key == "domain" || key == "gateway_domain" ? 256 : 4096;
            if (text.toUtf8().size() > limit || text.contains(QChar(0))) return {};
            if (key != "description") for (auto c : text) if (c.category() == QChar::Other_Control) return {};
        }
    }
    for (auto it = result.inheritance.begin(); it != result.inheritance.end(); ++it)
        if ((!defaults.contains(it.key()) && it.key() != "password") || !it.value().isBool()) return {};
    const auto e = result.effective(); const auto protocol = e.value("protocol").toString();
    if (!QStringList{"rdp", "ssh", "vnc"}.contains(protocol) ||
        !QStringList{"password", "private_key"}.contains(e.value("ssh_auth").toString()) ||
        !QStringList{"fit", "800x600", "1024x768", "1280x720", "1920x1080"}.contains(e.value("resolution").toString())) return {};
    const double port = e.value("port").toDouble(), colors = e.value("color_depth").toDouble();
    if (port < 1 || port > 65535 || port != int(port) || (colors != 16 && colors != 24 && colors != 32)) return {};
    const double idle = e.value("idle_minutes").toDouble(), gatewayPort = e.value("gateway_port").toDouble();
    if (idle < 0 || idle > 240 || idle != int(idle) || gatewayPort < 1 || gatewayPort > 65535 || gatewayPort != int(gatewayPort)) return {};
    if (!QStringList{"auto", "nla", "ext", "tls", "rdp"}.contains(e.value("security_protocol").toString()) ||
        !QStringList{"always", "required", "warn"}.contains(e.value("server_auth").toString()) ||
        !QStringList{"never", "always", "detect"}.contains(e.value("gateway_usage").toString()) ||
        !QStringList{"same", "separate"}.contains(e.value("gateway_credentials").toString()) ||
        !QStringList{"local", "remote", "none"}.contains(e.value("sound").toString())) return {};
    const auto username = e.value("username").toString(), domain = e.value("domain").toString();
    if (protocol == "rdp") {
        if (!domain.isEmpty() && (username.contains('\\') || username.contains('@'))) return {};
        for (auto c : domain) if (c.isSpace() || QString("\\/@:").contains(c)) return {};
    }
    return result;
}
