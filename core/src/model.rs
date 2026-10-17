use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use uuid::Uuid;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Protocol { Rdp, Ssh, Vnc, Unsupported }

impl Protocol {
    pub fn port(self) -> u16 { match self { Self::Rdp => 3389, Self::Ssh => 22, Self::Vnc => 5900, Self::Unsupported => 0 } }
    pub fn name(self) -> &'static str { match self { Self::Rdp => "rdp", Self::Ssh => "ssh", Self::Vnc => "vnc", Self::Unsupported => "unsupported" } }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SshAuth { #[default] Password, PrivateKey }
impl SshAuth {
    pub fn name(self) -> &'static str { match self { Self::Password => "password", Self::PrivateKey => "private_key" } }
    pub fn parse(value: &str) -> Result<Self, String> {
        match value { "password" => Ok(Self::Password), "private_key" => Ok(Self::PrivateKey), _ => Err("Invalid SSH authentication method".into()) }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind { Folder, Connection }

macro_rules! named_enum {
    ($name:ident, $error:literal, $default:ident, { $($variant:ident => $text:literal),+ $(,)? }) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
        pub enum $name { $(#[serde(rename = $text)] $variant),+ }
        impl Default for $name { fn default() -> Self { Self::$default } }
        impl $name {
            pub fn name(self) -> &'static str { match self { $(Self::$variant => $text),+ } }
            pub fn parse(value: &str) -> Result<Self, String> { match value { $($text => Ok(Self::$variant),)+ _ => Err($error.into()) } }
        }
    };
}
// RDP security negotiation mode. Auto keeps FreeRDP's negotiation defaults.
named_enum!(RdpSecurity, "Invalid RDP security protocol", Auto, { Auto => "auto", Nla => "nla", Ext => "ext", Tls => "tls", Rdp => "rdp" });
// Server identity (certificate) policy; mRemoteNG AuthenticationLevel.
named_enum!(ServerAuth, "Invalid server authentication policy", Warn, { Always => "always", Required => "required", Warn => "warn" });
named_enum!(GatewayUsage, "Invalid gateway usage", Never, { Never => "never", Always => "always", Detect => "detect" });
named_enum!(GatewayCredentials, "Invalid gateway credentials", Same, { Same => "same", Separate => "separate" });
// Remote audio; mRemoteNG RDPSounds (BringToThisComputer/LeaveAtRemoteComputer/DoNotPlay).
named_enum!(Sound, "Invalid sound redirection", Local, { Local => "local", Remote => "remote", None => "none" });
pub const MAX_IDLE_MINUTES: u16 = 240;

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum Resolution {
    #[serde(rename = "fit")]
    Fit,
    #[serde(rename = "800x600")]
    R800x600,
    #[default]
    #[serde(rename = "1024x768")]
    R1024x768,
    #[serde(rename = "1280x720")]
    R1280x720,
    #[serde(rename = "1920x1080")]
    R1920x1080,
}
impl Resolution {
    pub fn name(self) -> &'static str {
        match self { Self::Fit => "fit", Self::R800x600 => "800x600", Self::R1024x768 => "1024x768", Self::R1280x720 => "1280x720", Self::R1920x1080 => "1920x1080" }
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(try_from = "u8", into = "u8")]
pub enum Colors { C16, C24, #[default] C32 }
impl TryFrom<u8> for Colors {
    type Error = &'static str;
    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value { 16 => Ok(Self::C16), 24 => Ok(Self::C24), 32 => Ok(Self::C32), _ => Err("Colors must be 16, 24 or 32") }
    }
}
impl From<Colors> for u8 {
    fn from(value: Colors) -> Self { match value { Colors::C16 => 16, Colors::C24 => 24, Colors::C32 => 32 } }
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Overrides {
    pub protocol: Option<Protocol>,
    pub host: Option<String>,
    pub port: Option<u16>,
    pub username: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub description: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub icon: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub domain: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub console_session: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub resolution: Option<Resolution>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub automatic_resize: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub color_depth: Option<Colors>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub cache_bitmaps: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub wallpaper: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub themes: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub font_smoothing: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub security_protocol: Option<RdpSecurity>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub server_auth: Option<ServerAuth>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub use_credssp: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub idle_minutes: Option<u16>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub load_balance_info: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gateway_usage: Option<GatewayUsage>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gateway_host: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gateway_port: Option<u16>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gateway_username: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gateway_domain: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gateway_credentials: Option<GatewayCredentials>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sound: Option<Sound>,
    // Text clipboard sharing; mRemoteNG RedirectClipboard. Off unless chosen.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub clipboard: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub ssh_auth: Option<SshAuth>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub ssh_key_path: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub view_only: Option<bool>,
}
// Derived by the XML worker, not editable profile settings. Codes carry no
// source attribute values, plugin data or ciphertext into the Rust projection.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum NativeUnsupported {
    NodeType, Protocol, Port, Resolution, Colors, Inheritance, Preferences,
    Gateway, Authentication, Provider, Command, Redirection, DisplayOptions, IdleTimeout, Opaque,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NativeCapability { pub unsupported: Vec<NativeUnsupported> }

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub id: String,
    pub name: String,
    pub kind: Kind,
    pub parent: Option<String>,
    pub overrides: Overrides,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub native: Option<NativeCapability>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ProfileMove {
    pub id: String,
    pub parent: Option<String>,
    pub before: Option<String>,
}

/// One profile of an import batch. The caller chooses the new ids so it can
/// attach imported passwords; parents refer to the document or earlier entries.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ImportedProfile {
    pub id: String,
    pub name: String,
    pub kind: Kind,
    pub parent: Option<String>,
    pub overrides: Overrides,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NewProfile {
    pub name: String,
    pub kind: Kind,
    pub parent: Option<String>,
    pub overrides: Overrides,
}

pub fn validate_account(username: &str, domain: &str) -> Result<(), String> {
    if username.len() > 4096 || username.chars().any(char::is_control) { return Err("Invalid username".into()); }
    if domain.len() > 256 || domain.chars().any(|c| c.is_control() || c.is_whitespace())
        || domain.contains(['\\', '/', '@', ':']) { return Err("Invalid domain".into()); }
    if !domain.is_empty() && username.contains(['\\', '@']) {
        return Err("Use either a separate Domain and unqualified Username, or a qualified Username with an empty Domain".into());
    }
    Ok(())
}

fn validate_profile_account(protocol: Protocol, username: &str, domain: &str) -> Result<(), String> {
    // VNC has no account fields: imported RDP/SSH account attributes are passive.
    if protocol == Protocol::Vnc {
        if domain.len() > 4096 || domain.contains('\0') { return Err("Invalid passive domain".into()); }
        return Ok(());
    }
    if protocol != Protocol::Ssh { return validate_account(username, domain); }
    if username.len() > 4096 || username.chars().any(char::is_control) { return Err("Invalid SSH username".into()); }
    if domain.len() > 4096 || domain.contains('\0') { return Err("Invalid passive domain".into()); }
    Ok(())
}

/// Bounds for the RDP security/gateway settings. Cross-field security
/// compatibility (for example NLA without CredSSP) is reported at connect time
/// so inherited folder edits never make an entire file unloadable.
pub fn validate_rdp_security_overrides(o: &Overrides) -> Result<(), String> {
    if o.idle_minutes.is_some_and(|m| m > MAX_IDLE_MINUTES) { return Err("Minutes to Idle must be 0–240".into()); }
    if let Some(info) = &o.load_balance_info {
        if info.len() > 4096 || info.chars().any(char::is_control) { return Err("Load Balance Info must be at most 4096 bytes without control characters".into()); }
    }
    if o.gateway_port == Some(0) { return Err("Gateway port must be 1–65535".into()); }
    if let Some(host) = &o.gateway_host {
        if !host.is_empty() { validate_gateway_host(host)?; }
    }
    validate_account(o.gateway_username.as_deref().unwrap_or_default(), o.gateway_domain.as_deref().unwrap_or_default())
        .map_err(|e| format!("Gateway: {e}"))
}

pub fn validate_gateway_host(host: &str) -> Result<(), String> {
    if host.len() > 4096 || host.chars().any(|c| c.is_whitespace() || c.is_control())
        || host.contains(['/', '\\', '@', '#', '?', '[', ']'])
        || (host.contains(':') && host.parse::<std::net::Ipv6Addr>().is_err()) {
        return Err("Invalid gateway host".into());
    }
    Ok(())
}

pub fn validate_rdp_target(host: &str, port: u16, username: &str, domain: &str) -> Result<(), String> {
    if host.is_empty() || host.len() > 4096 || host.chars().any(|c| c.is_whitespace() || c.is_control())
        || host.contains(['/', '\\', '@', '#', '?', '[', ']'])
        || (host.contains(':') && host.parse::<std::net::Ipv6Addr>().is_err()) {
        return Err("Invalid RDP host".into());
    }
    if port == 0 { return Err("Port must be 1–65535".into()); }
    validate_account(username, domain)
}

pub fn validate_ssh_target(host: &str, port: u16, username: &str, key_path: &str) -> Result<(), String> {
    if host.is_empty() || host.len() > 4096 || host.chars().any(|c| c.is_whitespace() || c.is_control())
        || host.contains(['/', '\\', '@', '#', '?', '[', ']'])
        || (host.contains(':') && host.parse::<std::net::Ipv6Addr>().is_err()) { return Err("Invalid SSH host".into()); }
    if port == 0 { return Err("Port must be 1–65535".into()); }
    validate_profile_account(Protocol::Ssh, username, "")?;
    if key_path.len() > 4096 || key_path.chars().any(char::is_control) { return Err("Invalid SSH key-file path".into()); }
    Ok(())
}

pub fn validate_vnc_target(host: &str, port: u16) -> Result<(), String> {
    if host.is_empty() || host.len() > 4096 || host.chars().any(|c| c.is_whitespace() || c.is_control())
        || host.contains(['/', '\\', '@', '#', '?', '[', ']'])
        || (host.contains(':') && host.parse::<std::net::Ipv6Addr>().is_err()) { return Err("Invalid VNC host".into()); }
    if port == 0 { return Err("Port must be 1–65535".into()); }
    Ok(())
}

impl Profile {
    pub fn new(name: &str, kind: Kind, parent: Option<String>) -> Self {
        Self { id: Uuid::new_v4().to_string(), name: name.to_owned(), kind, parent, overrides: Overrides::default(), native: None }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Document {
    pub schema_version: u32,
    pub profiles: Vec<Profile>,
}

impl Default for Document {
    fn default() -> Self { Self { schema_version: 2, profiles: Vec::new() } }
}

#[derive(Debug, Clone, Serialize)]
pub struct Effective {
    pub protocol: Protocol,
    pub host: String,
    pub port: u16,
    pub username: String,
    pub description: String,
    pub icon: String,
    pub domain: String,
    pub console_session: bool,
    pub resolution: Resolution,
    pub automatic_resize: bool,
    pub color_depth: Colors,
    pub cache_bitmaps: bool,
    pub wallpaper: bool,
    pub themes: bool,
    pub font_smoothing: bool,
    pub security_protocol: RdpSecurity,
    pub server_auth: ServerAuth,
    pub use_credssp: bool,
    pub idle_minutes: u16,
    pub load_balance_info: String,
    pub gateway_usage: GatewayUsage,
    pub gateway_host: String,
    pub gateway_port: u16,
    pub gateway_username: String,
    pub gateway_domain: String,
    pub gateway_credentials: GatewayCredentials,
    pub sound: Sound,
    pub clipboard: bool,
    pub ssh_auth: SshAuth,
    pub ssh_key_path: String,
    pub view_only: bool,
    pub sources: HashMap<String, String>,
    pub unsupported: Vec<NativeUnsupported>,
}

impl Document {
    pub fn get(&self, id: &str) -> Result<&Profile, String> {
        self.profiles.iter().find(|p| p.id == id).ok_or_else(|| "Profile not found".to_owned())
    }

    pub fn validate(&self) -> Result<(), String> {
        if self.schema_version != 2 { return Err("Unsupported profile projection version".into()); }
        if self.profiles.len() > 100_000 { return Err("Too many profiles".into()); }
        let mut ids = HashSet::new();
        for p in &self.profiles {
            if Uuid::parse_str(&p.id).is_err() || !ids.insert(&p.id) { return Err("Invalid or duplicate profile ID".into()); }
            if p.name.trim().is_empty() || p.name.len() > 256 { return Err("Name must contain 1–256 bytes".into()); }
            if p.overrides.port == Some(0) { return Err("Port must be 1–65535".into()); }
            if let Some(native) = &p.native {
                let unique: HashSet<_> = native.unsupported.iter().collect();
                if native.unsupported.len() > 15 || unique.len() != native.unsupported.len() { return Err("Invalid native capability".into()); }
            }
            for s in [&p.overrides.host, &p.overrides.username].into_iter().flatten() {
                if s.len() > 4096 || s.contains('\0') { return Err("Invalid connection setting".into()); }
            }
            if let Some(description) = &p.overrides.description {
                if description.len() > 4096 || description.contains('\0') { return Err("Description must contain at most 4096 bytes and no NUL".into()); }
            }
            if let Some(icon) = &p.overrides.icon {
                if icon.len() > 256 || icon.chars().any(char::is_control) { return Err("Invalid icon name".into()); }
            }
            validate_rdp_security_overrides(&p.overrides)?;
            if let Some(path) = &p.overrides.ssh_key_path {
                if path.len() > 4096 || path.chars().any(char::is_control) { return Err("Invalid SSH key-file path".into()); }
            }
        }
        for p in &self.profiles {
            let mut seen = HashSet::new();
            seen.insert(p.id.as_str());
            let mut parent = p.parent.as_deref();
            while let Some(id) = parent {
                if !seen.insert(id) { return Err("Folder cycle is not allowed".into()); }
                if seen.len() > 128 { return Err("Folder nesting exceeds 128 levels".into()); }
                let folder = self.get(id)?;
                if folder.kind != Kind::Folder { return Err("Parent must be a folder".into()); }
                parent = folder.parent.as_deref();
            }
            let effective = self.effective(&p.id)?;
            validate_profile_account(effective.protocol, p.overrides.username.as_deref().unwrap_or_default(), p.overrides.domain.as_deref().unwrap_or_default())?;
            validate_profile_account(effective.protocol, &effective.username, &effective.domain)?;
        }
        Ok(())
    }

    pub fn effective(&self, id: &str) -> Result<Effective, String> {
        let unsupported = self.get(id)?.native.as_ref().map(|n| n.unsupported.clone()).unwrap_or_default();
        let mut chain = Vec::new();
        let mut current = Some(id);
        while let Some(id) = current {
            let p = self.get(id)?;
            chain.push(p);
            current = p.parent.as_deref();
            if chain.len() > 128 { return Err("Folder cycle or excessive depth".into()); }
        }
        let mut overrides = Overrides::default();
        let mut sources = HashMap::new();
        for p in chain.into_iter().rev() {
            macro_rules! apply {
                ($field:ident) => { if let Some(value) = &p.overrides.$field {
                    overrides.$field = Some(value.clone());
                    sources.insert(stringify!($field).into(), p.id.clone());
                }};
            }
            apply!(protocol); apply!(host); apply!(port); apply!(username); apply!(description); apply!(icon); apply!(domain);
            apply!(console_session); apply!(resolution); apply!(automatic_resize); apply!(color_depth);
            apply!(cache_bitmaps); apply!(wallpaper); apply!(themes); apply!(font_smoothing);
            apply!(security_protocol); apply!(server_auth); apply!(use_credssp); apply!(idle_minutes); apply!(load_balance_info);
            apply!(gateway_usage); apply!(gateway_host); apply!(gateway_port); apply!(gateway_username); apply!(gateway_domain); apply!(gateway_credentials); apply!(sound); apply!(clipboard);
            apply!(ssh_auth); apply!(ssh_key_path); apply!(view_only);
        }
        let protocol = overrides.protocol.unwrap_or(Protocol::Rdp);
        for field in ["protocol", "host", "port", "username", "description", "icon", "domain", "console_session", "resolution", "automatic_resize", "color_depth", "cache_bitmaps", "wallpaper", "themes", "font_smoothing", "security_protocol", "server_auth", "use_credssp", "idle_minutes", "load_balance_info", "gateway_usage", "gateway_host", "gateway_port", "gateway_username", "gateway_domain", "gateway_credentials", "sound", "clipboard", "ssh_auth", "ssh_key_path", "view_only"] {
            sources.entry(field.into()).or_insert("default".into());
        }
        Ok(Effective {
            protocol, host: overrides.host.unwrap_or_default(), port: overrides.port.unwrap_or_else(|| protocol.port()),
            username: overrides.username.unwrap_or_default(), description: overrides.description.unwrap_or_default(), icon: overrides.icon.unwrap_or_default(), domain: overrides.domain.unwrap_or_default(),
            console_session: overrides.console_session.unwrap_or(false), resolution: overrides.resolution.unwrap_or_default(), automatic_resize: overrides.automatic_resize.unwrap_or(true),
            color_depth: overrides.color_depth.unwrap_or_default(), cache_bitmaps: overrides.cache_bitmaps.unwrap_or(false),
            wallpaper: overrides.wallpaper.unwrap_or(true), themes: overrides.themes.unwrap_or(true), font_smoothing: overrides.font_smoothing.unwrap_or(true),
            security_protocol: overrides.security_protocol.unwrap_or_default(), server_auth: overrides.server_auth.unwrap_or_default(),
            use_credssp: overrides.use_credssp.unwrap_or(true), idle_minutes: overrides.idle_minutes.unwrap_or(0),
            load_balance_info: overrides.load_balance_info.unwrap_or_default(), gateway_usage: overrides.gateway_usage.unwrap_or_default(),
            gateway_host: overrides.gateway_host.unwrap_or_default(), gateway_port: overrides.gateway_port.unwrap_or(443),
            gateway_username: overrides.gateway_username.unwrap_or_default(), gateway_domain: overrides.gateway_domain.unwrap_or_default(),
            gateway_credentials: overrides.gateway_credentials.unwrap_or_default(), sound: overrides.sound.unwrap_or_default(), clipboard: overrides.clipboard.unwrap_or(false),
            ssh_auth: overrides.ssh_auth.unwrap_or_default(), ssh_key_path: overrides.ssh_key_path.unwrap_or_default(), view_only: overrides.view_only.unwrap_or(false), sources, unsupported,
        })
    }

    pub fn upsert(&mut self, profile: Profile) -> Result<(), String> {
        let mut candidate = self.clone();
        if let Some(p) = candidate.profiles.iter_mut().find(|p| p.id == profile.id) { *p = profile; }
        else { candidate.profiles.push(profile); }
        candidate.validate()?;
        *self = candidate;
        Ok(())
    }

    /// Reparents `id` and places it among its new siblings, before `before`
    /// or after the last existing sibling.
    pub fn relocate(&mut self, request: &ProfileMove) -> Result<(), String> {
        let index = self.profiles.iter().position(|p| p.id == request.id).ok_or("Profile not found")?;
        if let Some(before) = &request.before {
            if *before == request.id { return Err("Cannot place a profile before itself".into()); }
            if self.get(before)?.parent != request.parent { return Err("Insertion point must share the destination folder".into()); }
        }
        let mut candidate = self.clone();
        let mut profile = candidate.profiles.remove(index); profile.parent = request.parent.clone();
        let at = match &request.before {
            Some(before) => candidate.profiles.iter().position(|p| p.id == *before).ok_or("Profile not found")?,
            None => candidate.profiles.iter().rposition(|p| p.parent == request.parent).map_or(candidate.profiles.len(), |i| i + 1),
        };
        candidate.profiles.insert(at, profile);
        candidate.validate()?;
        *self = candidate;
        Ok(())
    }

    /// Appends an import batch in order; any invalid entry rejects the whole batch.
    pub fn import(&mut self, profiles: Vec<ImportedProfile>) -> Result<(), String> {
        if profiles.is_empty() { return Err("Nothing to import".into()); }
        if profiles.len() > 10_000 { return Err("Import exceeds 10000 profiles".into()); }
        let mut candidate = self.clone();
        for imported in profiles {
            if Uuid::parse_str(&imported.id).map(|u| u.hyphenated().to_string()) != Ok(imported.id.clone()) { return Err("Invalid imported profile id".into()); }
            if candidate.profiles.iter().any(|p| p.id == imported.id) { return Err("Imported profile id already exists".into()); }
            if let Some(parent) = &imported.parent { if candidate.get(parent)?.kind != Kind::Folder { return Err("Imported profiles must be placed in a folder".into()); } }
            candidate.profiles.push(Profile { id: imported.id, name: imported.name, kind: imported.kind, parent: imported.parent,
                                              overrides: imported.overrides, native: None });
        }
        candidate.validate()?;
        *self = candidate;
        Ok(())
    }

    /// Copies `id` and its descendants with new ids. The copy becomes the
    /// original's next sibling; native import findings stay with the original.
    pub fn duplicate(&mut self, id: &str) -> Result<String, String> {
        let subtree = self.descendants(id)?;
        let ids: HashMap<String, String> = subtree.iter().map(|old| (old.clone(), Uuid::new_v4().to_string())).collect();
        let copies: Vec<Profile> = self.profiles.iter().filter(|p| subtree.contains(&p.id)).map(|p| Profile {
            id: ids[&p.id].clone(),
            parent: if p.id == id { p.parent.clone() } else { p.parent.as_ref().map(|parent| ids[parent].clone()) },
            native: None, ..p.clone()
        }).collect();
        let at = self.profiles.iter().position(|p| p.id == id).ok_or("Profile not found")? + 1;
        let mut candidate = self.clone();
        candidate.profiles.splice(at..at, copies);
        candidate.validate()?;
        *self = candidate;
        Ok(ids[id].clone())
    }

    pub fn descendants(&self, id: &str) -> Result<HashSet<String>, String> {
        self.get(id)?;
        let mut removed = HashSet::from([id.to_owned()]);
        loop {
            let previous = removed.len();
            for p in &self.profiles {
                if p.parent.as_ref().is_some_and(|parent| removed.contains(parent)) { removed.insert(p.id.clone()); }
            }
            if previous == removed.len() { break; }
        }
        Ok(removed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn icon_inheritance_and_validation() {
        let mut d = Document::default();
        let mut folder = Profile::new("Icons", Kind::Folder, None);
        folder.overrides = serde_json::from_value(serde_json::json!({"icon":"SSH"})).unwrap();
        let child = Profile::new("Host", Kind::Connection, Some(folder.id.clone()));
        d.upsert(folder.clone()).unwrap(); d.upsert(child.clone()).unwrap();
        let e = serde_json::to_value(d.effective(&child.id).unwrap()).unwrap();
        assert_eq!(e["icon"], "SSH"); assert_eq!(e["sources"]["icon"], folder.id);
        let mut custom = child.clone();
        custom.overrides = serde_json::from_value(serde_json::json!({"icon":"Unknown imported icon"})).unwrap();
        d.upsert(custom.clone()).unwrap();
        assert_eq!(serde_json::to_value(d.effective(&custom.id).unwrap()).unwrap()["icon"], "Unknown imported icon");
        for icon in ["x".repeat(257), "é".repeat(129), "bad\nicon".into()] {
            custom.overrides = serde_json::from_value(serde_json::json!({"icon":icon})).unwrap();
            assert!(d.upsert(custom.clone()).is_err());
        }
        custom.overrides = serde_json::from_value(serde_json::json!({"icon":"é".repeat(128)})).unwrap();
        d.upsert(custom).unwrap();
    }
    #[test]
    fn relocate_orders_siblings_and_reparents() {
        let mut d = Document::default();
        let folder = Profile::new("Folder", Kind::Folder, None);
        let (a, b, c) = (Profile::new("A", Kind::Connection, None), Profile::new("B", Kind::Connection, None), Profile::new("C", Kind::Connection, None));
        for p in [&folder, &a, &b, &c] { d.upsert((*p).clone()).unwrap(); }
        let order = |d: &Document, parent: Option<&String>| d.profiles.iter().filter(|p| p.parent.as_ref() == parent).map(|p| p.name.clone()).collect::<Vec<_>>();
        d.relocate(&ProfileMove { id: c.id.clone(), parent: None, before: Some(a.id.clone()) }).unwrap();
        assert_eq!(order(&d, None), ["Folder", "C", "A", "B"]);
        d.relocate(&ProfileMove { id: a.id.clone(), parent: Some(folder.id.clone()), before: None }).unwrap();
        d.relocate(&ProfileMove { id: b.id.clone(), parent: Some(folder.id.clone()), before: Some(a.id.clone()) }).unwrap();
        assert_eq!(order(&d, Some(&folder.id)), ["B", "A"]); assert_eq!(order(&d, None), ["Folder", "C"]);
        d.relocate(&ProfileMove { id: c.id.clone(), parent: None, before: None }).unwrap();
        assert_eq!(order(&d, None), ["Folder", "C"]);
        assert!(d.relocate(&ProfileMove { id: c.id.clone(), parent: None, before: Some(a.id.clone()) }).is_err());
        assert!(d.relocate(&ProfileMove { id: folder.id.clone(), parent: Some(folder.id.clone()), before: None }).is_err());
        assert!(d.relocate(&ProfileMove { id: c.id.clone(), parent: Some(a.id.clone()), before: None }).is_err());
    }
    #[test]
    fn import_appends_batch_under_folder_or_rejects_it_whole() {
        let mut d = Document::default();
        let folder = Profile::new("Target", Kind::Folder, None); d.upsert(folder.clone()).unwrap();
        let (group, host) = (Uuid::new_v4().to_string(), Uuid::new_v4().to_string());
        let batch = |parent: Option<String>| serde_json::from_value::<Vec<ImportedProfile>>(serde_json::json!([
            {"id": group, "name": "Imported", "kind": "folder", "parent": parent, "overrides": {"protocol": null, "host": null, "port": null, "username": null}},
            {"id": host, "name": "Server", "kind": "connection", "parent": group, "overrides": {"protocol": "ssh", "host": "server.example", "port": 2222, "username": "root"}}])).unwrap();
        d.import(batch(Some(folder.id.clone()))).unwrap();
        assert_eq!(d.get(&group).unwrap().parent.as_deref(), Some(folder.id.as_str()));
        assert_eq!(d.effective(&host).unwrap().port, 2222);
        let before = d.profiles.len();
        assert!(d.import(batch(Some(folder.id.clone()))).is_err(), "duplicate ids");
        assert!(d.import(vec![]).is_err());
        assert_eq!(d.profiles.len(), before);
    }
    #[test]
    fn duplicate_copies_subtree_after_original() {
        let mut d = Document::default();
        let mut folder = Profile::new("Folder", Kind::Folder, None);
        folder.overrides.username = Some("alice".into());
        let child = Profile::new("Child", Kind::Connection, Some(folder.id.clone()));
        let after = Profile::new("After", Kind::Connection, None);
        for p in [&folder, &child, &after] { d.upsert((*p).clone()).unwrap(); }
        let copy = d.duplicate(&folder.id).unwrap();
        let roots: Vec<_> = d.profiles.iter().filter(|p| p.parent.is_none()).map(|p| p.id.clone()).collect();
        assert_eq!(roots, [folder.id.clone(), copy.clone(), after.id.clone()]);
        let copied = d.get(&copy).unwrap();
        assert_eq!(copied.name, "Folder"); assert_eq!(copied.overrides.username.as_deref(), Some("alice"));
        let children: Vec<_> = d.profiles.iter().filter(|p| p.parent.as_deref() == Some(copy.as_str())).collect();
        assert_eq!(children.len(), 1); assert_eq!(children[0].name, "Child"); assert_ne!(children[0].id, child.id);
        assert_eq!(d.profiles.iter().filter(|p| p.parent.as_deref() == Some(folder.id.as_str())).count(), 1);
        assert!(d.duplicate("missing").is_err());
    }
    #[test]
    fn inheritance_overrides_and_protocol_default_port() {
        let mut d = Document::default();
        let mut folder = Profile::new("Team", Kind::Folder, None);
        folder.overrides.username = Some("alice".into());
        folder.overrides.protocol = Some(Protocol::Ssh);
        let mut child = Profile::new("Host", Kind::Connection, Some(folder.id.clone()));
        d.upsert(folder).unwrap(); d.upsert(child.clone()).unwrap();
        let e = d.effective(&child.id).unwrap();
        assert_eq!(e.username, "alice"); assert_eq!(e.port, 22);
        assert_ne!(e.sources["username"], child.id);
        child.overrides.username = Some(String::new()); // Explicit empty value, not inheritance.
        child.overrides.protocol = Some(Protocol::Vnc);
        d.upsert(child.clone()).unwrap();
        assert_eq!(d.effective(&child.id).unwrap().username, "");
        assert_eq!(d.effective(&child.id).unwrap().port, 5900);
        child.overrides.username = None;
        d.upsert(child.clone()).unwrap();
        assert_eq!(d.effective(&child.id).unwrap().username, "alice");
    }
    #[test]
    fn reject_cycles_missing_parents_and_duplicate_ids() {
        let mut d = Document::default();
        let mut f = Profile::new("Folder", Kind::Folder, None);
        d.upsert(f.clone()).unwrap();
        f.parent = Some(f.id.clone());
        assert!(d.upsert(f.clone()).is_err());
        assert_eq!(d.get(&f.id).unwrap().parent, None);
        let mut duplicate = d.clone(); duplicate.profiles.push(d.profiles[0].clone());
        assert!(duplicate.validate().is_err());
        f.parent = Some(Uuid::new_v4().to_string()); assert!(d.upsert(f).is_err());
    }
}
