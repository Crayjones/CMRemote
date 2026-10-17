use remotemanager_core::{Core, bridge::{ffi::{ProfileChange, VncTarget}, normalize_vnc_target}};
use serde_json::{json, Value};

const FOLDER: &str = "1897a1fb-b702-45e9-bcae-884f59cc66c7";
const CHILD: &str = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
fn profile(id: &str, kind: &str, parent: Value, overrides: Value) -> Value {
    json!({"id":id,"name":"VNC fixture","kind":kind,"parent":parent,"overrides":overrides})
}
fn document(profiles: Vec<Value>) -> String { json!({"schema_version":2,"profiles":profiles}).to_string() }

#[test]
fn view_only_inherits_and_explicit_false_overrides_without_account_requirements() {
    let folder = profile(FOLDER, "folder", Value::Null, json!({"protocol":"vnc", "host":"localhost", "view_only":true,
        "username":"TEAM\\user@example.test", "domain":"passive domain/@:value"}));
    let mut child = profile(CHILD, "connection", json!(FOLDER), json!({}));
    let mut core = Core::detached(); core.load_document(&document(vec![folder.clone(), child.clone()])).unwrap();
    let snapshot: Value = serde_json::from_str(&core.snapshot().unwrap()).unwrap();
    assert_eq!(snapshot["profiles"][1]["effective"]["view_only"], true);
    assert_eq!(snapshot["profiles"][1]["effective"]["sources"]["view_only"], FOLDER);
    assert_eq!(snapshot["profiles"][1]["effective"]["port"], 5900);
    let view = core.open_session(CHILD, false).unwrap();
    assert!(view.view_only); assert!(view.graphical); assert!(!view.keyboard); assert!(!view.mouse);
    assert!(!view.clipboard); assert!(!view.resize); assert!(view.username.is_empty()); assert!(view.domain.is_empty());
    core.close_session(&view.id);
    child["overrides"]["view_only"] = json!(false);
    core.load_document(&document(vec![folder, child])).unwrap();
    let snapshot: Value = serde_json::from_str(&core.snapshot().unwrap()).unwrap();
    assert_eq!(snapshot["profiles"][1]["effective"]["view_only"], false);
    assert_eq!(snapshot["profiles"][1]["effective"]["sources"]["view_only"], CHILD);
    let control = core.open_session(CHILD, false).unwrap();
    assert!(!control.view_only); assert!(control.keyboard); assert!(control.mouse); core.close_session(&control.id);
    let stored = core.stored_document().unwrap();
    let mut reopened = Core::detached(); reopened.load_document(&stored).unwrap(); assert_eq!(reopened.stored_document().unwrap(), stored);
}

#[test]
fn temporary_vnc_is_secret_free_normalized_and_never_persists() {
    let mut core = Core::detached(); core.load_document(&document(vec![])).unwrap();
    let before = core.stored_document().unwrap();
    for host in ["LOCALHOST", "127.0.0.1", "2001:0db8::1", "::1"] {
        let target = VncTarget { host: host.into(), port: 5901, view_only: false };
        let normalized = normalize_vnc_target(&target).unwrap();
        let session = core.open_temporary_vnc_session(&target, false).unwrap();
        assert_eq!(session.host, normalized.host); assert_eq!(session.port, 5901); assert_eq!(session.protocol, "vnc");
        assert!(session.username.is_empty()); assert!(session.domain.is_empty()); assert!(!session.clipboard); assert!(!session.resize);
        assert!(session.graphical && session.keyboard && session.mouse); assert!(!session.view_only);
        assert_eq!(core.stored_document().unwrap(), before);
        core.session_state(&session.id, "disconnected").unwrap(); core.close_session(&session.id);
    }
    assert_eq!(core.session_count(), 0); assert_eq!(core.stored_document().unwrap(), before);
}

#[test]
fn invalid_vnc_targets_never_register_or_expose_values() {
    let mut core = Core::detached(); core.load_document(&document(vec![])).unwrap();
    let before = core.stored_document().unwrap();
    for (host, port) in [("", 5900), ("host", 0), ("bad host", 5900), ("host\n", 5900),
        ("bad:ipv6", 5900), ("[::1]", 5900), ("vnc://user:private-value@host", 5900), ("host/path", 5900)] {
        let target = VncTarget { host: host.into(), port, view_only: false };
        assert!(normalize_vnc_target(&target).is_err());
        for mock in [false, true] {
            let error = core.open_temporary_vnc_session(&target, mock).err().unwrap();
            assert!(!error.contains("private-value")); assert_eq!(core.session_count(), 0);
        }
    }
    let oversized = VncTarget { host: "x".repeat(4097), port: 5900, view_only: false };
    assert!(core.open_temporary_vnc_session(&oversized, false).is_err()); assert_eq!(core.stored_document().unwrap(), before);
}

#[test]
fn saved_registration_and_reconnect_are_file_scoped_and_keep_captured_info_immutable() {
    let source = document(vec![profile(CHILD, "connection", Value::Null, json!({"protocol":"vnc", "host":"localhost"}))]);
    let mut first = Core::detached(); first.load_document(&source).unwrap();
    let mut copied = Core::detached(); copied.load_document(&source).unwrap();
    let running = first.open_session(CHILD, false).unwrap();
    let independent = copied.open_session(CHILD, false).unwrap();
    assert_ne!(running.id, independent.id); assert_eq!(first.session_count(), 1); assert_eq!(copied.session_count(), 1);
    assert!(first.reopen_session(&running.id, false).is_err()); assert!(copied.reopen_session(&running.id, false).is_err());
    assert!(first.prepare_profile(ProfileChange::Delete, CHILD).is_err());
    let mut changed = profile(CHILD, "connection", Value::Null, json!({"protocol":"vnc", "host":"other.test", "port":5902, "view_only":true}));
    changed["name"] = json!("Edited target");
    let prepared = first.prepare_profile(ProfileChange::Update, &changed.to_string()).unwrap(); first.finish_profile(&prepared.token, true).unwrap();
    assert_eq!(running.host, "localhost"); assert_eq!(running.port, 5900); assert!(!running.view_only);
    first.session_state(&running.id, "disconnected").unwrap();
    let reconnected = first.reopen_session(&running.id, false).unwrap();
    assert_ne!(reconnected.id, running.id); assert_eq!(reconnected.host, "other.test"); assert_eq!(reconnected.port, 5902); assert!(reconnected.view_only);
    assert!(first.session_state(&running.id, "connected").is_err());
    assert_eq!(independent.host, "localhost"); assert!(!independent.view_only);
    first.close_session(&reconnected.id); assert_eq!(first.session_count(), 0); assert_eq!(copied.session_count(), 1);
    copied.close_session(&independent.id);
}

#[test]
fn view_only_mock_rejects_input_and_failed_reconnect_preserves_registration() {
    let mut core = Core::detached();
    let mut value = profile(CHILD, "connection", Value::Null, json!({"protocol":"vnc", "host":"localhost", "view_only":true}));
    core.load_document(&document(vec![value.clone()])).unwrap();
    let session = core.open_session(CHILD, true).unwrap(); assert!(core.mock_input(&session.id, "pointer press").is_err());
    core.session_state(&session.id, "disconnected").unwrap();
    value["native"] = json!({"unsupported":["authentication"]});
    // Native capability cannot be changed through a profile edit or substituted while the tab owns the document.
    assert!(core.prepare_profile(ProfileChange::Update, &value.to_string()).is_err());
    assert!(core.load_document(&document(vec![value])).is_err());
    assert!(core.reopen_session(&session.id, false).is_err()); assert_eq!(core.session_count(), 1);
    assert!(core.prepare_profile(ProfileChange::Delete, CHILD).is_err()); core.close_session(&session.id);
}

#[test]
fn invalid_saved_target_reconnect_keeps_disconnected_owning_registration() {
    let mut core = Core::detached();
    let mut value = profile(CHILD, "connection", Value::Null, json!({"protocol":"vnc", "host":"localhost"}));
    core.load_document(&document(vec![value.clone()])).unwrap();
    let session = core.open_session(CHILD, false).unwrap(); core.session_state(&session.id, "disconnected").unwrap();
    value["overrides"]["host"] = json!("vnc://user:private-value@host");
    let edit = core.prepare_profile(ProfileChange::Update, &value.to_string()).unwrap(); core.finish_profile(&edit.token, true).unwrap();
    let error = core.reopen_session(&session.id, false).err().unwrap(); assert!(!error.contains("private-value"));
    assert_eq!(core.session_count(), 1); assert_eq!(session.host, "localhost");
    assert!(core.prepare_profile(ProfileChange::Delete, CHILD).is_err());
    value["overrides"]["host"] = json!("::1");
    let edit = core.prepare_profile(ProfileChange::Update, &value.to_string()).unwrap(); core.finish_profile(&edit.token, true).unwrap();
    let replacement = core.reopen_session(&session.id, false).unwrap(); assert_eq!(replacement.host, "::1"); assert_eq!(core.session_count(), 1);
    core.close_session(&replacement.id);
}

#[test]
fn vnc_projection_rejects_passwords_and_untyped_options_without_echoing_secrets() {
    let mut core = Core::detached(); core.load_document(&document(vec![profile(CHILD, "connection", Value::Null, json!({"protocol":"vnc"}))])).unwrap();
    let snapshot: Value = serde_json::from_str(&core.snapshot().unwrap()).unwrap(); assert_eq!(snapshot["profiles"][0]["effective"]["view_only"], false);
    let before = core.stored_document().unwrap();
    for overrides in [json!({"protocol":"vnc", "password":"private-value"}), json!({"protocol":"vnc", "authentication":"private-value"}),
        json!({"protocol":"vnc", "view_only":"private-value"}), json!({"protocol":"vnc", "security":"private-value"})] {
        let error = core.load_document(&document(vec![profile(CHILD, "connection", Value::Null, overrides)])).unwrap_err();
        assert!(!error.contains("private-value")); assert_eq!(core.stored_document().unwrap(), before);
    }
}
