pub mod bridge;
pub mod codec;
pub mod credentials;
pub mod model;
pub mod projection;
pub mod session;
pub mod ssh;
pub mod vnc;

use bridge::ffi::{PreparedDocument, PreparedProfile, ProfileChange, RdpTarget, SshTarget, VncTarget, SessionEvent, SessionInfo};
use model::{Document, Kind, NewProfile, Profile, ProfileMove};
use session::{Command, Session};
use std::collections::HashMap;

pub struct Core {
    document: Document,
    loaded: bool,
    pending: Option<(String, Document)>,
    sessions: HashMap<String, Session>,
}

impl Core {
    pub fn detached() -> Self {
        Self { document: Document::default(), loaded: false, pending: None, sessions: HashMap::new() }
    }
    fn ready(&self) -> Result<(), String> {
        if !self.loaded { return Err("Connection file is not open".into()); }
        Ok(())
    }
    fn can_edit(&self) -> Result<(), String> {
        self.ready()?;
        if self.pending.is_some() { return Err("A profile save is pending".into()); }
        Ok(())
    }
    pub fn load_document(&mut self, json: &str) -> Result<(), String> {
        if self.pending.is_some() { return Err("A profile save is pending".into()); }
        if self.sessions.values().any(|s| s.profile.is_some()) { return Err("Close saved sessions before loading another document".into()); }
        let document = projection::decode(json.as_bytes())?;
        self.document = document; self.loaded = true; Ok(())
    }
    pub fn stored_document(&self) -> Result<String, String> { self.ready()?; projection::encode(&self.document) }
    pub fn prepare_profile(&mut self, change: ProfileChange, input: &str) -> Result<PreparedProfile, String> {
        self.can_edit()?;
        if input.len() > 16 * 1024 * 1024 { return Err("Profile request exceeds size limit".into()); }
        let (candidate, id) = match change {
            ProfileChange::Create => {
                let request: NewProfile = serde_json::from_str(input).map_err(|_| "Invalid new profile")?;
                let mut p = Profile::new(&request.name, request.kind, request.parent); p.overrides = request.overrides;
                let id = p.id.clone(); let mut candidate = self.document.clone(); candidate.upsert(p)?; (candidate, id)
            },
            ProfileChange::Update => self.edited_document(input)?,
            ProfileChange::Move => {
                let request: ProfileMove = serde_json::from_str(input).map_err(|_| "Invalid profile move")?;
                let mut candidate = self.document.clone(); candidate.relocate(&request)?; (candidate, request.id)
            },
            ProfileChange::Import => {
                let request: Vec<model::ImportedProfile> = serde_json::from_str(input).map_err(|_| "Invalid import")?;
                let id = request.first().map(|p| p.id.clone()).unwrap_or_default();
                let mut candidate = self.document.clone(); candidate.import(request)?; (candidate, id)
            },
            ProfileChange::Duplicate => {
                let mut candidate = self.document.clone(); let id = candidate.duplicate(input)?; (candidate, id)
            },
            ProfileChange::Delete => {
                let removed = self.document.descendants(input)?;
                if self.sessions.values().any(|s| s.profile.as_ref().is_some_and(|p| removed.contains(p))) { return Err("Close all tabs in this folder before deleting it".into()); }
                let mut candidate = self.document.clone(); candidate.profiles.retain(|p| !removed.contains(&p.id)); (candidate, input.into())
            },
            _ => return Err("Unknown profile operation".into()),
        };
        let json = projection::encode(&candidate)?;
        let token = uuid::Uuid::new_v4().to_string(); self.pending = Some((token.clone(), candidate));
        Ok(PreparedProfile { token, id, json })
    }
    pub fn finish_profile(&mut self, token: &str, committed: bool) -> Result<(), String> {
        if self.pending.as_ref().map(|p| p.0.as_str()) != Some(token) { return Err("Unknown profile save token".into()); }
        let (_, document) = self.pending.take().unwrap();
        if committed { self.document = document; } Ok(())
    }
    pub fn prepare_document(&mut self) -> Result<PreparedDocument, String> {
        self.can_edit()?;
        let json = projection::encode(&self.document)?;
        let token = uuid::Uuid::new_v4().to_string(); self.pending = Some((token.clone(), self.document.clone()));
        Ok(PreparedDocument { token, json })
    }
    pub fn finish_document(&mut self, token: &str, json: &str) -> Result<(), String> {
        let (reserved_token, reserved) = self.pending.as_ref().ok_or("Unknown document save token")?;
        if reserved_token != token { return Err("Unknown document save token".into()); }
        let normalized = projection::decode(json.as_bytes())?;
        if normalized.schema_version != 2 || normalized.profiles.iter().any(|p| p.native.is_none()) {
            return Err("XML commit requires worker-derived native metadata".into());
        }
        let profiles: HashMap<_, _> = reserved.profiles.iter().map(|p| (p.id.as_str(), p)).collect();
        if normalized.profiles.len() != profiles.len() || normalized.profiles.iter().any(|p| {
            profiles.get(p.id.as_str()).is_none_or(|prior| prior.name != p.name || prior.kind != p.kind || prior.parent != p.parent)
        }) { return Err("XML commit does not match reserved profile identities".into()); }
        // The worker derives native defaults, inheritance and capability. Never
        // publish the pre-save draft in place of its validated committed projection.
        self.document = normalized; self.pending.take(); Ok(())
    }
    pub fn snapshot(&self) -> Result<String, String> {
        self.ready()?;
        let profiles: Result<Vec<_>, String> = self.document.profiles.iter().map(|p| {
            Ok(serde_json::json!({"id":p.id,"name":p.name,"kind":p.kind,"parent":p.parent,
                                  "overrides":p.overrides,"native":p.native,"effective":self.document.effective(&p.id)?}))
        }).collect();
        serde_json::to_string(&serde_json::json!({"schema_version":self.document.schema_version,"profiles":profiles?})).map_err(|_| "Cannot encode profile view".into())
    }
    fn edited_document(&self, json: &str) -> Result<(Document, String), String> {
        self.ready()?;
        let p: Profile = serde_json::from_str(json).map_err(|_| "Invalid profile edit")?;
        let original = self.document.get(&p.id)?;
        if original.kind != p.kind { return Err("Profile kind cannot be changed".into()); }
        if original.native != p.native { return Err("Native capability requires document-worker validation".into()); }
        let id = p.id.clone(); let mut candidate = self.document.clone(); candidate.upsert(p)?;
        Ok((candidate, id))
    }
    pub fn preview_profile(&self, json: &str) -> Result<String, String> {
        let (candidate, id) = self.edited_document(json)?;
        serde_json::to_string(&candidate.effective(&id)?).map_err(|_| "Cannot encode effective draft".into())
    }
    pub fn open_session(&mut self, id: &str, mock: bool) -> Result<SessionInfo, String> {
        self.can_edit()?;
        if self.sessions.values().any(|s| s.profile.as_deref() == Some(id)) { return Err("Profile already has an open tab".into()); }
        let p = self.document.get(id)?;
        if p.kind != Kind::Connection { return Err("Folders cannot open sessions".into()); }
        let (session, info) = Session::new(Some(id), &p.name, self.document.effective(id)?, mock)?;
        self.sessions.insert(info.id.clone(), session);
        Ok(info)
    }
    pub fn open_temporary_session(&mut self, target: &RdpTarget, mock: bool) -> Result<SessionInfo, String> {
        model::validate_rdp_target(&target.host, target.port, &target.username, &target.domain)?;
        let title = if target.host.contains(':') { format!("[{}]:{}", target.host, target.port) }
                    else { format!("{}:{}", target.host, target.port) };
        let effective = target.options.effective(target)?;
        let (session, info) = Session::new(None, &title, effective, mock)?;
        self.sessions.insert(info.id.clone(), session);
        Ok(info)
    }
    pub fn open_temporary_ssh_session(&mut self, target: &SshTarget, mock: bool) -> Result<SessionInfo, String> {
        let effective = target.effective()?;
        let title = if effective.host.contains(':') { format!("[{}]:{}", effective.host, effective.port) }
                    else { format!("{}:{}", effective.host, effective.port) };
        let (session, info) = Session::new(None, &title, effective, mock)?;
        self.sessions.insert(info.id.clone(), session);
        Ok(info)
    }
    pub fn open_temporary_vnc_session(&mut self, target: &VncTarget, mock: bool) -> Result<SessionInfo, String> {
        let effective = target.effective()?;
        let title = if effective.host.contains(':') { format!("[{}]:{}", effective.host, effective.port) }
                    else { format!("{}:{}", effective.host, effective.port) };
        let (session, info) = Session::new(None, &title, effective, mock)?;
        self.sessions.insert(info.id.clone(), session);
        Ok(info)
    }
    pub fn reopen_session(&mut self, id: &str, mock: bool) -> Result<SessionInfo, String> {
        self.can_edit()?;
        let previous = self.sessions.get(id).ok_or("Session is closed")?;
        if previous.state != "disconnected" || previous.mock != mock { return Err("Disconnect the owning session before reconnecting".into()); }
        let profile = previous.profile.as_deref().ok_or("Temporary sessions require their captured target")?;
        let p = self.document.get(profile)?;
        let (session, info) = Session::new(Some(profile), &p.name, self.document.effective(profile)?, mock)?;
        // Validation and construction precede publication. A failed reconnect
        // keeps the disconnected registration and its deletion/close guard.
        self.sessions.remove(id);
        self.sessions.insert(info.id.clone(), session);
        Ok(info)
    }
    pub fn session_state(&mut self, id: &str, state: &str) -> Result<(), String> {
        let s = self.sessions.get_mut(id).ok_or("Session is closed")?;
        let valid = match s.state.as_str() {
            "connecting" => matches!(state, "connected" | "failed" | "disconnected"),
            "connected" => matches!(state, "failed" | "disconnected"),
            "failed" => state == "disconnected",
            _ => false,
        };
        if !valid { return Err("Invalid session transition".into()); }
        s.state = state.into(); Ok(())
    }
    pub fn close_session(&mut self, id: &str) { self.sessions.remove(id); }
    pub fn session_count(&self) -> usize { self.sessions.len() }
    pub fn mock_input(&self, id: &str, description: &str) -> Result<(), String> {
        if description.len() > 256 { return Err("Input description too long".into()); }
        self.sessions.get(id).ok_or("Session is closed")?.command(Command::Input(description.into()))
    }
    pub fn mock_resize(&self, id: &str, width: u32, height: u32) -> Result<(), String> {
        self.sessions.get(id).ok_or("Session is closed")?.command(Command::Resize(width, height))
    }
    pub fn poll_events(&mut self) -> Vec<SessionEvent> { self.sessions.values_mut().flat_map(|s| s.poll()).collect() }
}

#[cfg(test)]
mod tests {
    use super::*;
    use model::Protocol;
    fn loaded() -> Core { let mut c = Core::detached(); c.load_document("{\"schema_version\":2,\"profiles\":[]}").unwrap(); c }
    fn create(c: &mut Core, name: &str, folder: bool, parent: Option<&str>) -> String {
        let request = serde_json::json!({"name":name,"kind":if folder {"folder"} else {"connection"},"parent":parent,"overrides":{}});
        let candidate = c.prepare_profile(ProfileChange::Create, &request.to_string()).unwrap();
        c.finish_profile(&candidate.token, true).unwrap(); candidate.id
    }
    fn update(c: &mut Core, profile: &Profile) {
        let candidate = c.prepare_profile(ProfileChange::Update, &serde_json::to_string(profile).unwrap()).unwrap();
        c.finish_profile(&candidate.token, true).unwrap();
    }
    #[test]
    fn draft_effective_preview_does_not_commit() {
        let mut c = loaded();
        let parent = create(&mut c, "Parent", true, None);
        let id = create(&mut c, "Child", false, Some(&parent));
        let mut folder = c.document.get(&parent).unwrap().clone(); folder.overrides.username = Some("inherited".into());
        update(&mut c, &folder);
        let before = c.snapshot().unwrap();
        let mut draft = c.document.get(&id).unwrap().clone(); draft.overrides.username = Some("".into());
        let effective: serde_json::Value = serde_json::from_str(&c.preview_profile(&serde_json::to_string(&draft).unwrap()).unwrap()).unwrap();
        assert_eq!(effective["username"], ""); assert_eq!(effective["sources"]["username"], id.as_str());
        draft.overrides.username = None; draft.overrides.protocol = Some(Protocol::Ssh);
        let effective: serde_json::Value = serde_json::from_str(&c.preview_profile(&serde_json::to_string(&draft).unwrap()).unwrap()).unwrap();
        assert_eq!(effective["username"], "inherited"); assert_eq!(effective["sources"]["username"], parent.as_str()); assert_eq!(effective["port"], 22);
        draft.parent = Some(id.clone()); assert!(c.preview_profile(&serde_json::to_string(&draft).unwrap()).is_err());
        assert_eq!(c.snapshot().unwrap(), before); assert_eq!(c.session_count(), 0);
    }
    #[test]
    fn tab_lifecycle_blocks_delete_reuses_profile_and_cancels_connect() {
        let mut c = loaded();
        let folder = create(&mut c, "Group", true, None);
        let id = create(&mut c, "Host", false, Some(&folder));
        let s = c.open_session(&id, true).unwrap();
        assert!(c.open_session(&id, true).is_err()); assert!(c.prepare_profile(ProfileChange::Delete, &folder).is_err());
        c.close_session(&s.id); assert_eq!(c.session_count(), 0);
        let t = c.open_session(&id, true).unwrap(); assert_ne!(s.id, t.id);
        c.close_session(&t.id); let deletion = c.prepare_profile(ProfileChange::Delete, &folder).unwrap(); c.finish_profile(&deletion.token, true).unwrap();
        assert!(c.document.profiles.is_empty());
    }
    #[test]
    fn failed_session_can_finish_cleanup_but_cannot_reactivate() {
        let mut c = loaded(); let id = create(&mut c, "Host", false, None);
        let session = c.open_session(&id, true).unwrap();
        c.session_state(&session.id, "connected").unwrap(); c.session_state(&session.id, "failed").unwrap();
        c.session_state(&session.id, "disconnected").unwrap(); assert!(c.session_state(&session.id, "connected").is_err());
        c.close_session(&session.id); assert!(c.session_state(&session.id, "connected").is_err());
    }
    #[test]
    #[allow(non_snake_case)]
    fn quickSessionsDoNotPersist() {
        for saved in [false, true] {
            let mut c = loaded(); if saved { create(&mut c, "Saved", false, None); }
            let snapshot = c.snapshot().unwrap();
            let target = RdpTarget { host: "127.0.0.1".into(), port: 3389, username: "alice".into(), domain: String::new(), options: Default::default() };
            let first = c.open_temporary_session(&target, true).unwrap(); assert!(c.sessions[&first.id].profile.is_none()); c.close_session(&first.id);
            let second = c.open_temporary_session(&target, true).unwrap(); assert_ne!(first.id, second.id);
            assert_eq!(second.host, target.host); assert_eq!(second.port, target.port); assert_eq!(second.username, target.username);
            c.session_state(&second.id, "disconnected").unwrap(); c.close_session(&second.id);
            assert_eq!(c.session_count(), 0); assert_eq!(c.snapshot().unwrap(), snapshot);
        }
    }
    #[test]
    #[allow(non_snake_case)]
    fn saveAsConnectionIsAtomic() {
        let mut c = loaded(); let folder = create(&mut c, "Team", true, None);
        let mut parent = c.document.get(&folder).unwrap().clone();
        parent.overrides.protocol = Some(Protocol::Ssh); parent.overrides.port = Some(22); parent.overrides.username = Some("inherited-user".into()); update(&mut c, &parent);
        let target = RdpTarget { host: "127.0.0.1".into(), port: 3390, username: String::new(), domain: String::new(), options: Default::default() };
        let session = c.open_temporary_session(&target, true).unwrap();
        let request = serde_json::json!({"name":"Complete", "kind":"connection", "parent":folder,
            "overrides":{"protocol":"rdp", "host":target.host, "port":target.port, "username":target.username}});
        let before = c.snapshot().unwrap();
        let aborted = c.prepare_profile(ProfileChange::Create, &request.to_string()).unwrap(); c.finish_profile(&aborted.token, false).unwrap(); assert_eq!(c.snapshot().unwrap(), before);
        for change in [serde_json::json!({"password":"UNSAVED-SECRET"}), serde_json::json!({"id":uuid::Uuid::new_v4().to_string()})] {
            let mut invalid = request.clone(); invalid.as_object_mut().unwrap().extend(change.as_object().unwrap().clone());
            let error = c.prepare_profile(ProfileChange::Create, &invalid.to_string()).err().unwrap(); assert!(!error.contains("UNSAVED-SECRET")); assert_eq!(c.snapshot().unwrap(), before);
        }
        let result = c.prepare_profile(ProfileChange::Create, &request.to_string()).unwrap(); c.finish_profile(&result.token, true).unwrap();
        assert_eq!(c.document.profiles.len(), 2); let effective = c.document.effective(&result.id).unwrap();
        assert_eq!(effective.host, target.host); assert_eq!(effective.port, target.port); assert_eq!(effective.username, target.username);
        assert_eq!(c.document.get(&result.id).unwrap().overrides.username, Some(String::new())); assert!(c.sessions[&session.id].profile.is_none());
        let mut reloaded = Core::detached(); reloaded.load_document(&c.stored_document().unwrap()).unwrap(); assert_eq!(c.document.profiles, reloaded.document.profiles);
        assert!(!c.stored_document().unwrap().contains("password")); c.close_session(&session.id);
    }
    #[test]
    #[allow(non_snake_case)]
    fn invalidTargetsLeaveNoSession() {
        let mut c = loaded(); let before = c.snapshot().unwrap();
        for (host, port, username) in [("", 3389, ""), ("host", 0, ""), ("host\n", 3389, ""),
            ("bad host", 3389, ""), ("rdp://user:UNSAVED-SECRET@host", 3389, ""), ("host", 3389, "user\0"),
            ("host", 3389, "user\n"), ("bad:ipv6", 3389, "")] {
            let target = RdpTarget { host: host.into(), port, username: username.into(), domain: String::new(), options: Default::default() };
            for mock in [false, true] { let error = c.open_temporary_session(&target, mock).err().unwrap(); assert!(!error.contains("UNSAVED-SECRET")); assert_eq!(c.session_count(), 0); assert_eq!(c.snapshot().unwrap(), before); }
        }
        for (host, username) in [("x".repeat(4097), String::new()), ("host".into(), "x".repeat(4097))] {
            let target = RdpTarget { host, port: 3389, username, domain: String::new(), options: Default::default() };
            assert!(c.open_temporary_session(&target, true).is_err()); assert_eq!(c.session_count(), 0);
        }
        for host in ["127.0.0.1", "server.example", "::1", "2001:db8::1"] {
            let target = RdpTarget { host: host.into(), port: 65535, username: "DOMAIN\\user".into(), domain: String::new(), options: Default::default() };
            let session = c.open_temporary_session(&target, false).unwrap(); c.close_session(&session.id);
        }
        assert_eq!(c.snapshot().unwrap(), before);
    }
}
