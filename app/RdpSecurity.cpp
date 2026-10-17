#include "RdpSecurity.h"
#include <freerdp/error.h>
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <freerdp/config.h>

QString rdpSecurityConflict(const RdpOptions& o) {
    const auto& mode = o.securityProtocol;
    if (!QStringList{"auto", "nla", "ext", "tls", "rdp"}.contains(mode)) return "Security Protocol has an unsupported value.";
    if (!QStringList{"always", "required", "warn"}.contains(o.serverAuth)) return "Server Authentication has an unsupported value.";
    if ((mode == "nla" || mode == "ext") && !o.useCredSsp)
        return QString("Security Protocol %1 requires Use CredSSP = Yes. Set Use CredSSP to Yes or choose TLS.").arg(mode == "nla" ? "NLA / CredSSP" : "Extended NLA");
    if (mode == "rdp" && o.serverAuth == "required")
        return "Legacy RDP Security cannot verify the server's identity, so it cannot be combined with Server Authentication \"Don't connect if authentication fails\". Choose TLS or NLA, or a different Server Authentication.";
    if (o.gatewayEnabled() && o.gatewayHost.trimmed().isEmpty()) return "Use RDP Gateway is enabled but Gateway Hostname is empty.";
    if (o.idleMinutes > 240) return "Minutes to Idle must be 0–240.";
    return {};
}
bool applyRdpSecurity(rdpSettings* s, const RdpOptions& o) {
    if (!s || !rdpSecurityConflict(o).isEmpty()) return false;
    const auto flags = [s](bool rdp, bool tls, bool nla, bool ext) {
        return freerdp_settings_set_bool(s, FreeRDP_RdpSecurity, rdp) && freerdp_settings_set_bool(s, FreeRDP_TlsSecurity, tls) &&
            freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, nla) && freerdp_settings_set_bool(s, FreeRDP_ExtSecurity, ext);
    };
    bool ok = freerdp_settings_set_bool(s, FreeRDP_NegotiateSecurityLayer, TRUE);
    // Explicit modes offer exactly one protocol; Automatic keeps FreeRDP's own
    // negotiation and only drops NLA when CredSSP is disabled (V101).
    if (o.securityProtocol == "nla") ok = ok && flags(false, false, true, false);
    else if (o.securityProtocol == "ext") ok = ok && flags(false, false, false, true);
    else if (o.securityProtocol == "tls") ok = ok && flags(false, true, false, false);
    else if (o.securityProtocol == "rdp") ok = ok && flags(true, false, false, false);
    else if (!o.useCredSsp) ok = ok && freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, FALSE) && freerdp_settings_set_bool(s, FreeRDP_ExtSecurity, FALSE);
    // Certificates: only "always" bypasses verification. Otherwise FreeRDP checks
    // the CA chain and its known_hosts store before asking the callbacks (V100).
    const bool bypass = o.serverAuth == "always";
    ok = ok && freerdp_settings_set_bool(s, FreeRDP_IgnoreCertificate, bypass) &&
        freerdp_settings_set_bool(s, FreeRDP_AutoAcceptCertificate, FALSE) &&
        freerdp_settings_set_bool(s, FreeRDP_AutoDenyCertificate, FALSE);
    if (!o.loadBalanceInfo.isEmpty()) {
        // Routing token bytes are passed verbatim; FreeRDP adds CRLF if absent.
        const auto token = o.loadBalanceInfo.toUtf8();
        ok = ok && freerdp_settings_set_pointer_len(s, FreeRDP_LoadBalanceInfo, token.constData(), size_t(token.size()));
    }
    if (!o.gatewayEnabled()) return ok;
    ok = ok && freerdp_set_gateway_usage_method(s, o.gatewayUsage == "detect" ? TSC_PROXY_MODE_DETECT : TSC_PROXY_MODE_DIRECT) &&
        freerdp_settings_set_string(s, FreeRDP_GatewayHostname, o.gatewayHost.trimmed().toUtf8().constData()) &&
        freerdp_settings_set_uint32(s, FreeRDP_GatewayPort, o.gatewayPort) &&
        freerdp_settings_set_bool(s, FreeRDP_GatewayUseSameCredentials, o.gatewayCredentials == "same");
    if (o.gatewayCredentials == "separate")
        ok = ok && freerdp_settings_set_string(s, FreeRDP_GatewayUsername, o.gatewayUsername.toUtf8().constData()) &&
            freerdp_settings_set_string(s, FreeRDP_GatewayDomain, o.gatewayDomain.toUtf8().constData());
    return ok;
}
bool rdpAudioPlaybackAvailable() {
#if defined(CHANNEL_RDPSND_CLIENT) && defined(CHANNEL_RDPDR_CLIENT)
    return true;
#else
    return false;
#endif
}
bool applyRdpAudio(rdpSettings* s, const QString& sound) {
    if (!s || !QStringList{"local", "remote", "none"}.contains(sound)) return false;
    // AudioPlayback makes the client loader add rdpsnd and register rdpdr (no devices).
    const bool local = sound == "local" && rdpAudioPlaybackAvailable();
    return freerdp_settings_set_bool(s, FreeRDP_AudioPlayback, local) &&
        freerdp_settings_set_bool(s, FreeRDP_RemoteConsoleAudio, sound == "remote");
}
bool requestRdpMessageChannel(rdpSettings* s) {
    // KRdp sends continuous RTT requests once active; FreeRDP fails the session
    // on any auto-detect request unless NetworkAutoDetect is enabled (B38).
    return s && freerdp_settings_set_bool(s, FreeRDP_SupportHeartbeatPdu, TRUE) &&
        freerdp_settings_set_bool(s, FreeRDP_NetworkAutoDetect, TRUE);
}
RdpCertificateDecision rdpCertificateDecision(const QString& serverAuth) {
    // Certificates FreeRDP cannot verify are accepted for this session only,
    // without a prompt, unless the profile requires server authentication.
    return serverAuth == "required" ? RdpCertificateDecision::Reject : RdpCertificateDecision::Session;
}
QString rdpFailureText(quint32 error, const RdpOptions& o, bool certificateRejected) {
    if (certificateRejected)
        return o.serverAuth == "required" ?
            "RDP server certificate is not CA-valid or previously trusted, and Server Authentication is \"Don't connect if authentication fails\"." :
            "RDP connection canceled: the server certificate was not trusted.";
    const QString name = QString::fromUtf8(freerdp_get_last_error_name(error));
    const QString mode = o.securityProtocol == "auto" ? "Automatic" : o.securityProtocol.toUpper();
    const QString where = o.gatewayEnabled() ? "server or RDP gateway" : "server";
    QString hint;
    switch (error) {
    case FREERDP_ERROR_CONNECT_HYBRID_REQUIRED_BY_SERVER:
        hint = "The server requires NLA. Set Security Protocol to NLA / CredSSP (or Automatic) with Use CredSSP = Yes."; break;
    case FREERDP_ERROR_SECURITY_NEGO_CONNECT_FAILED:
        hint = o.securityProtocol == "auto" ? "The server accepted none of the offered security protocols. Choose TLS or NLA explicitly in Security Protocol." :
            "The server does not accept Security Protocol " + mode + ". Choose a different Security Protocol."; break;
    case FREERDP_ERROR_TLS_CONNECT_FAILED: hint = "The TLS handshake failed. Check the " + where + "'s TLS configuration or choose a different Security Protocol."; break;
    case FREERDP_ERROR_AUTHENTICATION_FAILED: case FREERDP_ERROR_CONNECT_LOGON_FAILURE: case FREERDP_ERROR_CONNECT_WRONG_PASSWORD:
        hint = "Sign-in was rejected. Check the username, domain and password."; break;
    case FREERDP_ERROR_CONNECT_NO_OR_MISSING_CREDENTIALS: hint = "The " + where + " asked for credentials that were not supplied."; break;
    case FREERDP_ERROR_CONNECT_PASSWORD_EXPIRED: case FREERDP_ERROR_CONNECT_PASSWORD_MUST_CHANGE: case FREERDP_ERROR_CONNECT_PASSWORD_CERTAINLY_EXPIRED:
        hint = "The account password must be changed on the server."; break;
    case FREERDP_ERROR_CONNECT_ACCOUNT_DISABLED: case FREERDP_ERROR_CONNECT_ACCOUNT_LOCKED_OUT: case FREERDP_ERROR_CONNECT_ACCOUNT_EXPIRED:
    case FREERDP_ERROR_CONNECT_ACCOUNT_RESTRICTION: case FREERDP_ERROR_CONNECT_CLIENT_REVOKED:
        hint = "The server rejected this account."; break;
    case FREERDP_ERROR_SERVER_DENIED_CONNECTION: case FREERDP_ERROR_CONNECT_ACCESS_DENIED: case FREERDP_ERROR_INSUFFICIENT_PRIVILEGES:
        hint = "The server denied remote access for this account."; break;
    case FREERDP_ERROR_DNS_NAME_NOT_FOUND: hint = "The " + where + " host name was not found."; break;
    case FREERDP_ERROR_CONNECT_FAILED: hint = "Could not reach the " + where + ". Check the host name, port and network."; break;
    case FREERDP_ERROR_CONNECT_TRANSPORT_FAILED:
        hint = "The connection closed during setup. The server may have rejected the credentials, the security settings or the session."; break;
    case FREERDP_ERROR_CONNECT_KDC_UNREACHABLE: hint = "The Kerberos KDC is unreachable."; break;
    case FREERDP_ERROR_CONNECT_TARGET_BOOTING: hint = "The server is still starting. Try again shortly."; break;
    case FREERDP_ERROR_CONNECT_CANCELLED: hint = "The connection was canceled."; break;
    default: hint = "Check the network, credentials and Security Protocol."; break;
    }
    return "RDP connection failed: " + hint + " (" + name + ")";
}
