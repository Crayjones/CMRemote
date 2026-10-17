#pragma once
#include <QString>

// English Language.resx resources from mRemoteNG. Keys match
// AbstractConnectionRecord's LocalizedDescription attributes.
inline QString configDescription(const QString& key) {
    struct Description { const char* field; const char* resource; const char* text; };
    static const Description descriptions[] = {
        {"name", "PropertyDescriptionName", "This is the name that will be displayed in the connections tree."},
        {"description", "PropertyDescriptionDescription", "Put your notes or a description for the host here."},
        {"icon", "PropertyDescriptionIcon", "Choose a icon that will be displayed when connected to the host."},
        {"host", "PropertyDescriptionHostnameIp", "Enter the hostname or ip you want to connect to."},
        {"username", "PropertyDescriptionUsername", "Enter your username."},
        {"password", "PropertyDescriptionPassword", "Enter your password."},
        {"domain", "PropertyDescriptionDomain", "Enter your domain."},
        {"protocol", "PropertyDescriptionProtocol", "Choose the protocol mRemoteNG should use to connect to the host."},
        {"port", "PropertyDescriptionPort", "Enter the port the selected protocol is listening on."},
        {"console_session", "PropertyDescriptionUseConsoleSession", "Connect to the console session of the remote host."},
        {"server_auth", "PropertyDescriptionAuthenticationLevel", "Unverified or changed certificates are accepted for this session without a prompt or saving trust. This does not protect against server impersonation. Choose Don't connect if authentication fails to reject unverified certificates."},
        {"use_credssp", "PropertyDescriptionUseCredSsp", "Use the Credential Security Support Provider (CredSSP) for authentication if it is available."},
        {"idle_minutes", "PropertyDescriptionRDPMinutesToIdleTimeout", "The number of minutes for the RDP session to sit idle before automatically disconnecting (for no limit use 0)"},
        {"load_balance_info", "PropertyDescriptionLoadBalanceInfo", "Specifies the load balancing information for use by load balancing routers to choose the best server."},
        {"sound", "PropertyDescriptionRedirectSounds", "Select how remote sound should be redirected."},
        {"clipboard", "PropertyDescriptionRedirectClipboard", "Select whether the clipboard should be shared."},
        {"gateway_usage", "PropertyDescriptionRdpGatewayUsageMethod", "Specifies when to use a Remote Desktop Gateway (RD Gateway) server."},
        {"gateway_host", "PropertyDescriptionRDGatewayHostname", "Specifies the host name of the Remote Desktop Gateway server."},
        {"gateway_credentials", "PropertyDescriptionRDGatewayUseConnectionCredentials", "Specifies whether or not to log on to the gateway using the same username and password as the connection."},
        {"gateway_username", "PropertyDescriptionRDGatewayUsername", "Specifies the user name that a user provides to connect to the RD Gateway server."},
        {"gateway_domain", "PropertyDescriptionRDGatewayDomain", "Specifies the domain name that a user provides to connect to the RD Gateway server."},
        {"resolution", "PropertyDescriptionResolution", "Choose the resolution or mode this connection will open in."},
        {"automatic_resize", "PropertyDescriptionAutomaticResize", "Select whether to automatically resize the connection when the window is resized or when fullscreen mode is toggled. Requires RDC 8.0 or higher."},
        {"color_depth", "PropertyDescriptionColors", "Select the color quality to be used."},
        {"cache_bitmaps", "PropertyDescriptionCacheBitmaps", "Select whether to use bitmap caching or not."},
        {"wallpaper", "PropertyDescriptionDisplayWallpaper", "Select yes if the wallpaper of the remote host should be displayed."},
        {"themes", "PropertyDescriptionDisplayThemes", "Select yes if the theme of the remote host should be displayed."},
        {"font_smoothing", "PropertyDescriptionEnableFontSmoothing", "Select whether to use font smoothing or not."},
        {"smart_size", "PropertyDescriptionSmartSizeMode", "Select the SmartSize mode to be used."},
        {"view_only", "PropertyDescriptionViewOnly", "If you want to establish a view only connection to the host select yes."},
        {"inherit_all", "PropertyDescriptionAll", "Toggle all inheritance options."}
    };
    for (const auto& description : descriptions) if (key == description.field) return QString::fromUtf8(description.text);
    return {};
}
