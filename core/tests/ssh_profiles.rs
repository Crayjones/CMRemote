use remotemanager_core::{Core, bridge::{ffi::SshTarget, normalize_ssh_target}};
use serde_json::{json, Value};

const FOLDER: &str = "1897a1fb-b702-45e9-bcae-884f59cc66c7";
const CHILD: &str = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
fn profile(id: &str, kind: &str, parent: Value, overrides: Value) -> Value {
    json!({"id":id,"name":"SSH fixture","kind":kind,"parent":parent,"overrides":overrides})
}
fn document(profiles: Vec<Value>) -> String { json!({"schema_version":2,"profiles":profiles}).to_string() }

#[test]
#[allow(non_snake_case)]
fn sshAuthAndKeyPathInheritWithExplicitEmptyOverride() {
    let mut core = Core::detached();
    let folder = profile(FOLDER, "folder", Value::Null, json!({"protocol":"ssh", "host":"localhost",
        "ssh_auth":"private_key", "ssh_key_path":"/owned/fixture/id_ed25519"}));
    let mut child = profile(CHILD, "connection", json!(FOLDER), json!({"username":"literal-user"}));
    core.load_document(&document(vec![folder.clone(), child.clone()])).unwrap();
    let snapshot: Value = serde_json::from_str(&core.snapshot().unwrap()).unwrap();
    let effective = &snapshot["profiles"][1]["effective"];
    assert_eq!(effective["ssh_auth"], "private_key");
    assert_eq!(effective["ssh_key_path"], "/owned/fixture/id_ed25519");
    assert_eq!(effective["sources"]["ssh_auth"], FOLDER);
    assert_eq!(effective["sources"]["ssh_key_path"], FOLDER);
    assert_eq!(effective["port"], 22);
    child["overrides"]["ssh_auth"] = json!("password");
    child["overrides"]["ssh_key_path"] = json!("");
    core.load_document(&document(vec![folder, child])).unwrap();
    let snapshot: Value = serde_json::from_str(&core.snapshot().unwrap()).unwrap();
    assert_eq!(snapshot["profiles"][1]["effective"]["ssh_auth"], "password");
    assert_eq!(snapshot["profiles"][1]["effective"]["ssh_key_path"], "");
    assert_eq!(snapshot["profiles"][1]["effective"]["sources"]["ssh_key_path"], CHILD);
    let stored = core.stored_document().unwrap();
    let mut reopened = Core::detached(); reopened.load_document(&stored).unwrap();
    assert_eq!(reopened.stored_document().unwrap(), stored);
}

#[test]
#[allow(non_snake_case)]
fn sshLiteralUsernameAndPassiveDomainUsesNoRdpComposition() {
    let mut core = Core::detached();
    for username in ["user@example.test", "TEAM\\literal-user", "ユーザー"] {
        let source = document(vec![profile(CHILD, "connection", Value::Null,
            json!({"protocol":"ssh","host":"localhost","username":username,"domain":"passive domain/@:value"}))]);
        core.load_document(&source).unwrap();
        let session = core.open_session(CHILD, true).unwrap();
        assert_eq!(session.username, username);
        let snapshot: Value = serde_json::from_str(&core.snapshot().unwrap()).unwrap();
        assert_eq!(snapshot["profiles"][0]["effective"]["domain"], "passive domain/@:value");
        core.close_session(&session.id);
    }
    assert!(core.load_document(&document(vec![profile(CHILD, "connection", Value::Null,
        json!({"protocol":"rdp","username":"user@example.test","domain":"TEAM"}))])).is_err());
}

#[test]
#[allow(non_snake_case)]
fn temporarySshNeverPersistsAndSaveAsDoesNotRetarget() {
    let mut core = Core::detached();
    core.load_document(&document(vec![])).unwrap();
    let original = core.stored_document().unwrap();
    let target = SshTarget { host: "LOCALHOST".into(), port: 22, username: "TEAM\\literal-user".into(),
        auth: "private_key".into(), key_path: "/owned/fixture/id_ed25519".into() };
    let normalized = normalize_ssh_target(&target).unwrap(); assert_eq!(normalized.host, "localhost");
    assert_eq!(normalized.username, target.username);
    let first = core.open_temporary_ssh_session(&normalized, true).unwrap();
    assert_eq!(first.protocol, "ssh"); assert_eq!(first.ssh_auth, "private_key"); assert_eq!(first.ssh_key_path, target.key_path);
    assert!(!first.graphical); assert!(!first.mouse); assert!(!first.clipboard); assert!(first.keyboard); assert!(first.resize);
    assert!(first.domain.is_empty()); assert_eq!(core.stored_document().unwrap(), original);
    let candidate = core.prepare_profile(remotemanager_core::bridge::ffi::ProfileChange::Create,
        &json!({"name":"Saved SSH", "kind":"connection", "parent":null,
            "overrides":{"protocol":"ssh", "host":"other.test", "port":2222, "username":"other-user", "ssh_auth":"password", "ssh_key_path":""}}).to_string()).unwrap();
    core.finish_profile(&candidate.token, true).unwrap();
    assert_eq!(first.host, "localhost"); assert_eq!(first.port, 22); assert_eq!(first.username, target.username);
    core.session_state(&first.id, "disconnected").unwrap(); core.close_session(&first.id);
    let second = core.open_temporary_ssh_session(&normalized, true).unwrap(); assert_ne!(first.id, second.id);
    assert_eq!(second.host, normalized.host); core.close_session(&second.id); assert_eq!(core.session_count(), 0);
    for (host, auth, key_path) in [("bad/host", "password", ""), ("localhost", "agent", ""), ("localhost", "private_key", "bad\u{0}path")] {
        let invalid = SshTarget { host: host.into(), port: 22, username: "literal-user".into(), auth: auth.into(), key_path: key_path.into() };
        assert!(normalize_ssh_target(&invalid).is_err()); assert!(core.open_temporary_ssh_session(&invalid, true).is_err());
        assert_eq!(core.session_count(), 0);
    }
}

#[test]
#[allow(non_snake_case)]
fn realSshRegistrationsStaySecretFreeAndFileScoped() {
    let source = document(vec![profile(CHILD, "connection", Value::Null,
        json!({"protocol":"ssh", "host":"localhost", "username":"TEAM\\literal", "domain":"inactive domain"}))]);
    let mut first = Core::detached(); first.load_document(&source).unwrap();
    let mut copied = Core::detached(); copied.load_document(&source).unwrap();
    let running = first.open_session(CHILD, false).unwrap();
    let independent = copied.open_session(CHILD, false).unwrap();
    assert_ne!(running.id, independent.id); assert_eq!(running.protocol, "ssh");
    assert!(!running.graphical); assert!(!running.clipboard); assert!(running.keyboard);
    assert!(first.mock_input(&running.id, "not accepted").is_err());
    assert_eq!(first.session_count(), 1); assert_eq!(copied.session_count(), 1);
    first.session_state(&running.id, "disconnected").unwrap(); assert_eq!(first.session_count(), 1);
    assert_eq!(first.stored_document().unwrap(), copied.stored_document().unwrap());
    first.close_session(&running.id); assert_eq!(first.session_count(), 0); assert_eq!(copied.session_count(), 1);
    copied.close_session(&independent.id);
}

#[test]
#[allow(non_snake_case)]
fn sshMetadataIsBoundedTypedAndContainsNoAuthenticationSecret() {
    let mut core = Core::detached();
    core.load_document(&document(vec![profile(CHILD, "connection", Value::Null,
        json!({"protocol":"ssh","ssh_auth":"password","ssh_key_path":""}))])).unwrap();
    let original = core.stored_document().unwrap();
    for overrides in [json!({"protocol":"ssh","ssh_auth":"agent"}),
        json!({"protocol":"ssh","ssh_key_path":"x".repeat(4097)}),
        json!({"protocol":"ssh","ssh_key_path":"bad\u{0}path"}),
        json!({"protocol":"ssh","password":"synthetic authentication secret"}),
        json!({"protocol":"ssh","passphrase":"synthetic authentication secret"}),
        json!({"protocol":"ssh","private_key":"synthetic authentication secret"})] {
        let error = core.load_document(&document(vec![profile(CHILD, "connection", Value::Null, overrides)])).unwrap_err();
        assert!(!error.contains("synthetic authentication secret"));
        assert_eq!(core.stored_document().unwrap(), original);
    }
}
