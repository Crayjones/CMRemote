#[cxx::bridge(namespace = "rm")]
pub mod ffi {
    #[derive(Debug, PartialEq, Eq)]
    enum ProfileChange { Create, Update, Delete, Move, Duplicate, Import }
    struct PreparedProfile { token: String, id: String, json: String }
    struct PreparedDocument { token: String, json: String }
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct RdpOptions {
        resolution: String,
        automatic_resize: bool,
        color_depth: u8,
        console_session: bool,
        cache_bitmaps: bool,
        wallpaper: bool,
        themes: bool,
        font_smoothing: bool,
        security_protocol: String,
        server_auth: String,
        use_credssp: bool,
        idle_minutes: u16,
        load_balance_info: String,
        gateway_usage: String,
        gateway_host: String,
        gateway_port: u16,
        gateway_username: String,
        gateway_domain: String,
        gateway_credentials: String,
        sound: String,
        clipboard: bool,
    }
    struct RdpTarget { host: String, port: u16, username: String, domain: String, options: RdpOptions }
    struct SshTarget { host: String, port: u16, username: String, auth: String, key_path: String }
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct VncTarget { host: String, port: u16, view_only: bool }
    struct SessionInfo {
        id: String,
        title: String,
        protocol: String,
        host: String,
        port: u16,
        username: String,
        domain: String,
        options: RdpOptions,
        ssh_auth: String,
        ssh_key_path: String,
        view_only: bool,
        mock: bool,
        graphical: bool,
        keyboard: bool,
        mouse: bool,
        clipboard: bool,
        resize: bool,
    }
    struct SessionEvent { id: String, kind: String, text: String, width: u32, height: u32 }
    extern "Rust" {
        type Core;
        fn detached_core() -> Box<Core>;
        fn load_document(self: &mut Core, json: &str) -> Result<()>;
        fn stored_document(self: &Core) -> Result<String>;
        fn prepare_profile(self: &mut Core, change: ProfileChange, input: &str) -> Result<PreparedProfile>;
        fn finish_profile(self: &mut Core, token: &str, committed: bool) -> Result<()>;
        fn prepare_document(self: &mut Core) -> Result<PreparedDocument>;
        fn finish_document(self: &mut Core, token: &str, json: &str) -> Result<()>;
        fn core_version() -> String;
        fn validate_document(json: &str) -> Result<()>;
        fn decompress_bzip2(data: &[u8], limit: usize) -> Result<Vec<u8>>;
        fn normalize_rdp_target(target: &RdpTarget) -> Result<RdpTarget>;
        fn normalize_ssh_target(target: &SshTarget) -> Result<SshTarget>;
        fn normalize_vnc_target(target: &VncTarget) -> Result<VncTarget>;
        fn snapshot(self: &Core) -> Result<String>;
        fn preview_profile(self: &Core, json: &str) -> Result<String>;
        fn open_session(self: &mut Core, id: &str, mock: bool) -> Result<SessionInfo>;
        fn open_temporary_session(self: &mut Core, target: &RdpTarget, mock: bool) -> Result<SessionInfo>;
        fn open_temporary_ssh_session(self: &mut Core, target: &SshTarget, mock: bool) -> Result<SessionInfo>;
        fn open_temporary_vnc_session(self: &mut Core, target: &VncTarget, mock: bool) -> Result<SessionInfo>;
        fn reopen_session(self: &mut Core, id: &str, mock: bool) -> Result<SessionInfo>;
        fn session_state(self: &mut Core, id: &str, state: &str) -> Result<()>;
        fn close_session(self: &mut Core, id: &str);
        fn session_count(self: &Core) -> usize;
        fn mock_input(self: &Core, id: &str, description: &str) -> Result<()>;
        fn mock_resize(self: &Core, id: &str, width: u32, height: u32) -> Result<()>;
        fn poll_events(self: &mut Core) -> Vec<SessionEvent>;
    }
}

use crate::Core;
pub fn detached_core() -> Box<Core> { Box::new(Core::detached()) }
pub fn validate_document(json: &str) -> Result<(), String> { crate::projection::decode(json.as_bytes()).map(|_| ()) }
pub fn decompress_bzip2(data: &[u8], limit: usize) -> Result<Vec<u8>, String> { crate::codec::decompress_bzip2(data, limit) }
pub fn core_version() -> String { format!("RemoteManager Rust core {}", env!("CARGO_PKG_VERSION")) }
pub fn normalize_rdp_target(target: &ffi::RdpTarget) -> Result<ffi::RdpTarget, String> {
    crate::model::validate_rdp_target(&target.host, target.port, &target.username, &target.domain)?;
    target.options.validate()?;
    let host = target.host.parse::<std::net::IpAddr>().map(|address| address.to_string())
        .unwrap_or_else(|_| target.host.to_ascii_lowercase());
    Ok(ffi::RdpTarget { host, port: target.port, username: target.username.clone(), domain: target.domain.clone(), options: target.options.clone() })
}

pub fn normalize_ssh_target(target: &ffi::SshTarget) -> Result<ffi::SshTarget, String> {
    crate::model::validate_ssh_target(&target.host, target.port, &target.username, &target.key_path)?;
    crate::model::SshAuth::parse(&target.auth)?;
    let host = target.host.parse::<std::net::IpAddr>().map(|address| address.to_string())
        .unwrap_or_else(|_| target.host.to_ascii_lowercase());
    Ok(ffi::SshTarget { host, port: target.port, username: target.username.clone(), auth: target.auth.clone(), key_path: target.key_path.clone() })
}
impl ffi::SshTarget {
    pub fn effective(&self) -> Result<crate::model::Effective, String> {
        let target = normalize_ssh_target(self)?;
        let mut profile = crate::model::Profile::new("Temporary SSH", crate::model::Kind::Connection, None);
        profile.overrides = crate::model::Overrides { protocol: Some(crate::model::Protocol::Ssh), host: Some(target.host), port: Some(target.port),
            username: Some(target.username), ssh_auth: Some(crate::model::SshAuth::parse(&target.auth)?), ssh_key_path: Some(target.key_path), ..Default::default() };
        let id = profile.id.clone();
        crate::model::Document { schema_version: 2, profiles: vec![profile] }.effective(&id)
    }
}

pub fn normalize_vnc_target(target: &ffi::VncTarget) -> Result<ffi::VncTarget, String> {
    crate::model::validate_vnc_target(&target.host, target.port)?;
    let host = target.host.parse::<std::net::IpAddr>().map(|address| address.to_string())
        .unwrap_or_else(|_| target.host.to_ascii_lowercase());
    Ok(ffi::VncTarget { host, port: target.port, view_only: target.view_only })
}
impl ffi::VncTarget {
    pub fn effective(&self) -> Result<crate::model::Effective, String> {
        let target = normalize_vnc_target(self)?;
        let mut profile = crate::model::Profile::new("Temporary VNC", crate::model::Kind::Connection, None);
        profile.overrides = crate::model::Overrides { protocol: Some(crate::model::Protocol::Vnc), host: Some(target.host), port: Some(target.port),
            view_only: Some(target.view_only), ..Default::default() };
        let id = profile.id.clone();
        crate::model::Document { schema_version: 2, profiles: vec![profile] }.effective(&id)
    }
}

impl Default for ffi::RdpOptions {
    fn default() -> Self {
        Self { resolution: "1024x768".into(), automatic_resize: true, color_depth: 32,
            console_session: false, cache_bitmaps: false, wallpaper: true, themes: true, font_smoothing: true,
            security_protocol: "auto".into(), server_auth: "warn".into(), use_credssp: true, idle_minutes: 0, load_balance_info: String::new(),
            gateway_usage: "never".into(), gateway_host: String::new(), gateway_port: 443, gateway_username: String::new(),
            gateway_domain: String::new(), gateway_credentials: "same".into(), sound: "local".into(), clipboard: false }
    }
}
impl ffi::RdpOptions {
    fn resolution(&self) -> Result<crate::model::Resolution, String> {
        serde_json::from_value(serde_json::Value::String(self.resolution.clone())).map_err(|_| "Invalid RDP resolution".into())
    }
    fn colors(&self) -> Result<crate::model::Colors, String> { self.color_depth.try_into().map_err(str::to_owned) }
    fn overrides(&self) -> Result<crate::model::Overrides, String> {
        use crate::model::*;
        Ok(Overrides {
            security_protocol: Some(RdpSecurity::parse(&self.security_protocol)?), server_auth: Some(ServerAuth::parse(&self.server_auth)?),
            use_credssp: Some(self.use_credssp), idle_minutes: Some(self.idle_minutes), load_balance_info: Some(self.load_balance_info.clone()),
            gateway_usage: Some(GatewayUsage::parse(&self.gateway_usage)?), gateway_host: Some(self.gateway_host.clone()),
            gateway_port: Some(self.gateway_port), gateway_username: Some(self.gateway_username.clone()),
            gateway_domain: Some(self.gateway_domain.clone()), gateway_credentials: Some(GatewayCredentials::parse(&self.gateway_credentials)?),
            sound: Some(Sound::parse(&self.sound)?), clipboard: Some(self.clipboard),
            ..Default::default()
        })
    }
    pub fn validate(&self) -> Result<(), String> {
        self.resolution()?; self.colors()?;
        crate::model::validate_rdp_security_overrides(&self.overrides()?)
    }
    pub fn effective(&self, target: &ffi::RdpTarget) -> Result<crate::model::Effective, String> {
        Ok(crate::model::Effective {
            protocol: crate::model::Protocol::Rdp, host: target.host.clone(), port: target.port,
            username: target.username.clone(), domain: target.domain.clone(), description: String::new(), icon: String::new(),
            resolution: self.resolution()?, color_depth: self.colors()?, automatic_resize: self.automatic_resize,
            console_session: self.console_session, cache_bitmaps: self.cache_bitmaps, wallpaper: self.wallpaper,
            themes: self.themes, font_smoothing: self.font_smoothing,
            security_protocol: crate::model::RdpSecurity::parse(&self.security_protocol)?, server_auth: crate::model::ServerAuth::parse(&self.server_auth)?,
            use_credssp: self.use_credssp, idle_minutes: self.idle_minutes, load_balance_info: self.load_balance_info.clone(),
            gateway_usage: crate::model::GatewayUsage::parse(&self.gateway_usage)?, gateway_host: self.gateway_host.clone(), gateway_port: self.gateway_port,
            gateway_username: self.gateway_username.clone(), gateway_domain: self.gateway_domain.clone(),
            gateway_credentials: crate::model::GatewayCredentials::parse(&self.gateway_credentials)?,
            sound: crate::model::Sound::parse(&self.sound)?, clipboard: self.clipboard,
            ssh_auth: crate::model::SshAuth::Password, ssh_key_path: String::new(), view_only: false, sources: std::collections::HashMap::new(), unsupported: Vec::new(),
        })
    }
}
impl From<&crate::model::Effective> for ffi::RdpOptions {
    fn from(e: &crate::model::Effective) -> Self {
        Self { resolution: e.resolution.name().into(), automatic_resize: e.automatic_resize, color_depth: e.color_depth.into(),
            console_session: e.console_session, cache_bitmaps: e.cache_bitmaps, wallpaper: e.wallpaper, themes: e.themes, font_smoothing: e.font_smoothing,
            security_protocol: e.security_protocol.name().into(), server_auth: e.server_auth.name().into(), use_credssp: e.use_credssp,
            idle_minutes: e.idle_minutes, load_balance_info: e.load_balance_info.clone(), gateway_usage: e.gateway_usage.name().into(),
            gateway_host: e.gateway_host.clone(), gateway_port: e.gateway_port, gateway_username: e.gateway_username.clone(),
            gateway_domain: e.gateway_domain.clone(), gateway_credentials: e.gateway_credentials.name().into(), sound: e.sound.name().into(), clipboard: e.clipboard }
    }
}
