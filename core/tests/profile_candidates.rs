use remotemanager_core::{bridge::ffi::ProfileChange, Core};
use serde_json::{json, Value};

#[test]
#[allow(non_snake_case)]
fn detachedCoreUsesPreparedCommitsV73() {
    let mut core = Core::detached(); assert!(core.snapshot().is_err());
    assert!(core.load_document(r#"{"schema_version":1,"profiles":[]}"#).is_err());
    core.load_document(r#"{"schema_version":2,"profiles":[]}"#).unwrap();
    let before = core.snapshot().unwrap();
    let aborted = core.prepare_profile(ProfileChange::Create, &request("Aborted", None)).unwrap();
    assert_eq!(core.snapshot().unwrap(), before);
    core.finish_profile(&aborted.token, false).unwrap(); assert_eq!(core.snapshot().unwrap(), before);
    let prepared = core.prepare_profile(ProfileChange::Create, &request("Committed", None)).unwrap();
    core.finish_document(&prepared.token, &normalized_document(&prepared.json).to_string()).unwrap();
    let saved = core.stored_document().unwrap();
    let target = remotemanager_core::bridge::ffi::RdpTarget { host: "127.0.0.1".into(), port: 3389, username: String::new(), domain: String::new(), options: Default::default() };
    let temporary = core.open_temporary_session(&target, true).unwrap(); core.close_session(&temporary.id);
    assert_eq!(core.stored_document().unwrap(), saved);
    let mut reloaded = Core::detached(); reloaded.load_document(&saved).unwrap();
    assert_eq!(reloaded.snapshot().unwrap(), core.snapshot().unwrap());
}

fn empty() -> &'static str { r#"{"schema_version":2,"profiles":[]}"# }
fn request(name: &str, parent: Option<&str>) -> String {
    json!({"name": name, "kind": "connection", "parent": parent,
           "overrides": {"host": "127.0.0.1", "username": "", "protocol": "rdp", "port": 3389}}).to_string()
}
fn loaded() -> Core { let mut core = Core::detached(); core.load_document(empty()).unwrap(); core }

#[test]
#[allow(non_snake_case)]
fn preparedChangesDoNotMutateOrWrite() {
    let mut core = loaded(); let before = core.snapshot().unwrap();
    let prepared = core.prepare_profile(ProfileChange::Create, &request("Prepared", None)).unwrap();
    assert_eq!(core.snapshot().unwrap(), before);
    let candidate: Value = serde_json::from_str(&prepared.json).unwrap();
    assert_eq!(candidate["profiles"][0]["id"], prepared.id); assert!(candidate["profiles"][0].get("effective").is_none());
    core.finish_profile(&prepared.token, false).unwrap(); assert_eq!(core.snapshot().unwrap(), before);
}

#[test]
#[allow(non_snake_case)]
fn preparedCommitAndAbortPreserveIds() {
    let mut core = loaded();
    let created = core.prepare_profile(ProfileChange::Create, &request("Child", None)).unwrap();
    core.finish_profile(&created.token, true).unwrap();
    let document: Value = serde_json::from_str(&core.stored_document().unwrap()).unwrap();
    let mut edited = document["profiles"][0].clone(); edited["name"] = json!("Renamed");
    let before = core.snapshot().unwrap();
    let draft = core.prepare_profile(ProfileChange::Update, &edited.to_string()).unwrap();
    assert_eq!(draft.id, created.id); core.finish_profile(&draft.token, false).unwrap();
    assert_eq!(core.snapshot().unwrap(), before);
    let saved = core.prepare_profile(ProfileChange::Update, &edited.to_string()).unwrap();
    core.finish_profile(&saved.token, true).unwrap();
    let mut reopened = Core::detached(); reopened.load_document(&core.stored_document().unwrap()).unwrap();
    assert_eq!(reopened.snapshot().unwrap(), core.snapshot().unwrap());
    let view: Value = serde_json::from_str(&reopened.snapshot().unwrap()).unwrap();
    assert_eq!(view["profiles"][0]["id"], created.id); assert_eq!(view["profiles"][0]["overrides"]["username"], "");
    let deletion = core.prepare_profile(ProfileChange::Delete, &created.id).unwrap();
    core.finish_profile(&deletion.token, true).unwrap();
    assert_eq!(serde_json::from_str::<Value>(&core.snapshot().unwrap()).unwrap()["profiles"], json!([]));
}

#[test]
#[allow(non_snake_case)]
fn pendingCandidateBlocksConflictingMutations() {
    let mut core = loaded();
    let first = core.prepare_profile(ProfileChange::Create, &request("First", None)).unwrap();
    let mut other = loaded(); let foreign = other.prepare_profile(ProfileChange::Create, &request("Other", None)).unwrap();
    assert!(core.finish_profile(&foreign.token, true).is_err());
    assert!(core.prepare_profile(ProfileChange::Create, &request("Second", None)).is_err());
    assert!(core.load_document(empty()).is_err());
    core.finish_profile(&first.token, true).unwrap();
    let session = core.open_session(&first.id, true).unwrap();
    assert!(core.prepare_profile(ProfileChange::Delete, &first.id).is_err());
    core.close_session(&session.id);
    let deletion = core.prepare_profile(ProfileChange::Delete, &first.id).unwrap();
    assert!(core.open_session(&first.id, true).is_err());
    core.finish_profile(&deletion.token, false).unwrap(); assert!(core.open_session(&first.id, true).is_ok());
}

#[test]
#[allow(non_snake_case)]
fn savedReconnectReplacesRegistrationOnlyAfterValidation() {
    let mut core = loaded();
    let created = core.prepare_profile(ProfileChange::Create, &request("Owned", None)).unwrap();
    core.finish_profile(&created.token, true).unwrap();
    let original = core.open_session(&created.id, true).unwrap();
    assert!(core.reopen_session(&original.id, true).is_err());
    core.session_state(&original.id, "disconnected").unwrap();
    let mut document: Value = serde_json::from_str(&core.stored_document().unwrap()).unwrap();
    let mut profile = document["profiles"][0].clone(); profile["overrides"]["host"] = json!("127.0.0.2");
    let pending = core.prepare_profile(ProfileChange::Update, &profile.to_string()).unwrap();
    assert!(core.reopen_session(&original.id, true).is_err()); assert_eq!(core.session_count(), 1);
    core.finish_profile(&pending.token, true).unwrap();
    let replacement = core.reopen_session(&original.id, true).unwrap();
    assert_ne!(original.id, replacement.id); assert_eq!(replacement.host, "127.0.0.2"); assert_eq!(core.session_count(), 1);
    assert!(core.reopen_session(&original.id, true).is_err());
    core.session_state(&replacement.id, "disconnected").unwrap();
    document = serde_json::from_str(&core.stored_document().unwrap()).unwrap();
    profile = document["profiles"][0].clone(); profile["overrides"]["protocol"] = json!("ssh");
    let pending = core.prepare_profile(ProfileChange::Update, &profile.to_string()).unwrap(); core.finish_profile(&pending.token, true).unwrap();
    assert!(core.reopen_session(&replacement.id, false).is_err()); assert_eq!(core.session_count(), 1);
    assert!(core.prepare_profile(ProfileChange::Delete, &created.id).is_err());
    core.close_session(&replacement.id); assert_eq!(core.session_count(), 0);
}

fn normalized_document(candidate: &str) -> Value {
    let mut document: Value = serde_json::from_str(candidate).unwrap(); document["schema_version"] = json!(2);
    for profile in document["profiles"].as_array_mut().unwrap() { profile["native"] = json!({"unsupported": []}); }
    document
}

#[test]
#[allow(non_snake_case)]
fn workerNormalizedCommitReconcilesOnlyOwningCandidate() {
    let mut first = loaded(); let mut second = loaded();
    let prepared = first.prepare_profile(ProfileChange::Create, &request("First", None)).unwrap();
    let foreign = second.prepare_profile(ProfileChange::Create, &request("Second", None)).unwrap();
    let before = first.snapshot().unwrap(); let other = second.snapshot().unwrap();
    let mut normalized = normalized_document(&prepared.json);
    normalized["profiles"][0]["overrides"]["resolution"] = json!("fit");
    normalized["profiles"][0]["overrides"]["wallpaper"] = json!(false);
    assert!(first.finish_document(&foreign.token, &normalized.to_string()).is_err()); assert_eq!(first.snapshot().unwrap(), before);
    first.finish_document(&prepared.token, &normalized.to_string()).unwrap();
    assert_eq!(serde_json::from_str::<Value>(&first.stored_document().unwrap()).unwrap(), normalized);
    assert_eq!(second.snapshot().unwrap(), other); assert!(first.finish_document(&prepared.token, &normalized.to_string()).is_err());
    second.finish_profile(&foreign.token, false).unwrap();
    let active = first.open_session(&prepared.id, true).unwrap(); assert_eq!(active.host, "127.0.0.1");
    let mut edit = normalized["profiles"][0].clone(); edit["overrides"]["host"] = json!("localhost");
    let changed = first.prepare_profile(ProfileChange::Update, &edit.to_string()).unwrap();
    let mut committed = normalized_document(&changed.json); committed["profiles"][0]["native"]["unsupported"] = json!(["opaque"]);
    first.finish_document(&changed.token, &committed.to_string()).unwrap(); assert_eq!(first.session_count(), 1);
    assert_eq!(active.host, "127.0.0.1"); first.close_session(&active.id);
    assert!(first.open_session(&prepared.id, true).is_err()); // worker-derived capability survives finalization
}

#[test]
#[allow(non_snake_case)]
fn fileOnlyReservationBlocksEditsAndCanAbortWithoutMetadataChange() {
    let mut core = loaded(); let before = core.snapshot().unwrap();
    let prepared = core.prepare_document().unwrap(); assert_eq!(prepared.json, core.stored_document().unwrap());
    assert!(core.prepare_document().is_err()); assert!(core.prepare_profile(ProfileChange::Create, &request("Blocked", None)).is_err());
    assert!(core.load_document(empty()).is_err()); assert_eq!(core.snapshot().unwrap(), before);
    core.finish_profile(&prepared.token, false).unwrap(); assert_eq!(core.snapshot().unwrap(), before);
    let prepared = core.prepare_document().unwrap();
    core.finish_document(&prepared.token, &normalized_document(&prepared.json).to_string()).unwrap();
    assert_eq!(serde_json::from_str::<Value>(&core.stored_document().unwrap()).unwrap(), json!({"schema_version":2,"profiles":[]}));
}

#[test]
#[allow(non_snake_case)]
fn normalizedCommitCannotReplaceReservedIdentityOrPublishInvalidData() {
    let mut core = loaded(); let before = core.snapshot().unwrap();
    let prepared = core.prepare_profile(ProfileChange::Create, &request("Reserved", None)).unwrap();
    let valid = normalized_document(&prepared.json);
    let mut changed_id = valid.clone(); changed_id["profiles"][0]["id"] = json!("e6683f1f-4e06-4200-9a17-89cd30c68b12");
    let mut changed_name = valid.clone(); changed_name["profiles"][0]["name"] = json!("Foreign");
    let mut changed_kind = valid.clone(); changed_kind["profiles"][0]["kind"] = json!("folder");
    let mut deleted = valid.clone(); deleted["profiles"] = json!([]);
    let mut untyped = valid.clone(); untyped["profiles"][0]["native"] = Value::Null;
    let mut secret = valid.clone(); secret["profiles"][0]["password"] = json!("synthetic-reconciliation-secret");
    for invalid in [changed_id, changed_name, changed_kind, deleted, untyped, secret] {
        let error = core.finish_document(&prepared.token, &invalid.to_string()).unwrap_err();
        assert!(!error.contains("synthetic-reconciliation-secret")); assert_eq!(core.snapshot().unwrap(), before);
        assert!(core.prepare_document().is_err()); // failed reconciliation retains reservation for recovery
    }
    assert!(core.finish_document(&prepared.token, "{bad").is_err());
    core.finish_document(&prepared.token, &valid.to_string()).unwrap();
    assert_eq!(serde_json::from_str::<Value>(&core.stored_document().unwrap()).unwrap(), valid);
}

#[test]
#[allow(non_snake_case)]
fn invalidDocumentNeverLoads() {
    let mut core = Core::detached(); assert!(core.snapshot().is_err());
    for input in ["{bad", r#"{"schema_version":99,"profiles":[]}"#,
                  r#"{"schema_version":2,"profiles":[],"password":"synthetic-secret"}"#] {
        let error = core.load_document(input).unwrap_err(); assert!(!error.contains("synthetic-secret"));
        assert!(core.snapshot().is_err()); assert!(core.prepare_profile(ProfileChange::Create, &request("Bad", None)).is_err());
    }
    core.load_document(empty()).unwrap(); let before = core.snapshot().unwrap();
    assert!(core.load_document("{bad").is_err()); assert_eq!(core.snapshot().unwrap(), before);
    let mut oversized = empty().to_string(); oversized.push_str(&" ".repeat(16 * 1024 * 1024));
    assert!(core.load_document(&oversized).is_err()); assert_eq!(core.snapshot().unwrap(), before);
}
