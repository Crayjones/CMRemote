#![allow(non_snake_case)]
use remotemanager_core::{bridge::ffi::ProfileChange, Core};
use serde_json::{json, Value};

const ID: &str = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
fn document() -> Value { json!({"schema_version":2,"profiles":[{"id":ID,"name":"Connection","kind":"connection","parent":null,"overrides":{"host":"localhost","username":"alice","protocol":"rdp","port":3389}}]}) }
fn profile(core: &Core, id: &str) -> Value {
    let view: Value = serde_json::from_str(&core.snapshot().unwrap()).unwrap();
    let mut profile = view["profiles"].as_array().unwrap().iter().find(|p| p["id"] == id).unwrap().clone();
    profile.as_object_mut().unwrap().remove("effective"); profile
}
fn effective(core: &Core, id: &str) -> Value {
    let view: Value = serde_json::from_str(&core.snapshot().unwrap()).unwrap();
    view["profiles"].as_array().unwrap().iter().find(|p| p["id"] == id).unwrap()["effective"].clone()
}
#[test]
fn currentProjectionDefaults() {
    let mut core = Core::detached(); core.load_document(&document().to_string()).unwrap();
    let e = effective(&core, ID); assert_eq!(e["domain"], ""); assert_eq!(e["description"], ""); assert_eq!(e["resolution"], "1024x768");
    assert_eq!(e["automatic_resize"], true); assert_eq!(e["color_depth"], 32); assert_eq!(e["console_session"], false);
    assert_eq!(e["wallpaper"], true); assert_eq!(e["themes"], true); assert_eq!(e["font_smoothing"], true);
    assert_eq!(serde_json::from_str::<Value>(&core.stored_document().unwrap()).unwrap()["schema_version"], 2);
}
#[test]
fn preparedSettingsAbortAndCommitPreserveIds() {
    let mut core = Core::detached(); let original = document().to_string(); core.load_document(&original).unwrap();
    let mut edit = profile(&core, ID); edit["overrides"]["domain"] = json!("EXAMPLE");
    let candidate = core.prepare_profile(ProfileChange::Update, &edit.to_string()).unwrap();
    assert_eq!(serde_json::from_str::<Value>(&candidate.json).unwrap()["schema_version"], 2); assert_eq!(candidate.id, ID);
    assert_eq!(serde_json::from_str::<Value>(&core.stored_document().unwrap()).unwrap()["schema_version"], 2);
    core.finish_profile(&candidate.token, false).unwrap(); assert_eq!(effective(&core, ID)["domain"], "");
    let candidate = core.prepare_profile(ProfileChange::Update, &edit.to_string()).unwrap(); core.finish_profile(&candidate.token, true).unwrap();
    assert_eq!(effective(&core, ID)["domain"], "EXAMPLE"); let committed = core.stored_document().unwrap();
    let mut restarted = Core::detached(); restarted.load_document(&committed).unwrap(); assert_eq!(profile(&restarted, ID)["id"], ID);
    edit["overrides"]["domain"] = Value::Null; let candidate = restarted.prepare_profile(ProfileChange::Update, &edit.to_string()).unwrap();
    assert_eq!(serde_json::from_str::<Value>(&candidate.json).unwrap()["schema_version"], 2);
    restarted.finish_profile(&candidate.token, true).unwrap(); assert_eq!(effective(&restarted, ID)["domain"], "");
}
#[test]
fn newSettingsInheritAndRoundTrip() {
    let mut core = Core::detached(); core.load_document(&document().to_string()).unwrap();
    let options = json!({"description":"Folder description","domain":"EXAMPLE","console_session":true,"resolution":"1280x720","automatic_resize":false,"color_depth":16,"cache_bitmaps":true,"wallpaper":false,"themes":false,"font_smoothing":false});
    let candidate = core.prepare_profile(ProfileChange::Create, &json!({"name":"Defaults","kind":"folder","parent":null,"overrides":options}).to_string()).unwrap();
    let parent = candidate.id.clone(); core.finish_profile(&candidate.token, true).unwrap();
    let mut edit = profile(&core, ID); edit["parent"] = json!(parent); let candidate = core.prepare_profile(ProfileChange::Update, &edit.to_string()).unwrap(); core.finish_profile(&candidate.token, true).unwrap();
    let inherited = effective(&core, ID); for (key, value) in options.as_object().unwrap() { assert_eq!(&inherited[key], value); assert_eq!(inherited["sources"][key], parent); }
    edit["overrides"]["domain"] = json!(""); edit["overrides"]["description"] = json!(""); edit["overrides"]["automatic_resize"] = json!(true);
    let candidate = core.prepare_profile(ProfileChange::Update, &edit.to_string()).unwrap(); core.finish_profile(&candidate.token, true).unwrap();
    let mut restart = Core::detached(); restart.load_document(&core.stored_document().unwrap()).unwrap(); let local = effective(&restart, ID);
    assert_eq!(local["domain"], ""); assert_eq!(local["description"], ""); assert_eq!(local["automatic_resize"], true); assert_eq!(local["sources"]["domain"], ID);
    edit["overrides"]["domain"] = Value::Null; let candidate = restart.prepare_profile(ProfileChange::Update, &edit.to_string()).unwrap(); restart.finish_profile(&candidate.token, true).unwrap();
    assert_eq!(effective(&restart, ID)["domain"], "EXAMPLE");
}
#[test]
fn invalidSettingsPreserveProjection() {
    let mut core = Core::detached(); core.load_document(&document().to_string()).unwrap(); let before = core.snapshot().unwrap();
    for (field, value) in [("domain", json!("x".repeat(257))), ("domain", json!("EXAMPLE\\bad")), ("description", json!("x".repeat(4097))), ("description", json!("bad\u{0}")), ("resolution", json!("99999x99999")), ("color_depth", json!(8)), ("console_session", json!("yes")), ("automatic_resize", json!(1)), ("cache_bitmaps", json!("true"))] {
        let mut edit = profile(&core, ID); edit["overrides"][field] = value; assert!(core.prepare_profile(ProfileChange::Update, &edit.to_string()).is_err());
        assert_eq!(core.snapshot().unwrap(), before);
    }
    for username in ["EXAMPLE\\alice", "alice@example.test"] {
        let mut edit = profile(&core, ID); edit["overrides"]["username"] = json!(username); edit["overrides"]["domain"] = json!("EXAMPLE");
        assert!(core.prepare_profile(ProfileChange::Update, &edit.to_string()).is_err()); assert_eq!(core.snapshot().unwrap(), before);
    }
    for username in ["EXAMPLE\\alice", "alice@example.test"] {
        let mut inherited = document(); inherited["schema_version"] = json!(2);
        inherited["profiles"][0]["overrides"]["username"] = json!(username);
        let parent = "d05a929e-cbf4-46f1-9d38-dcaec9a1ae0d";
        inherited["profiles"][0]["parent"] = json!(parent);
        inherited["profiles"].as_array_mut().unwrap().push(json!({"id":parent,"name":"Domain defaults","kind":"folder","parent":null,"overrides":{"domain":"EXAMPLE"}}));
        let mut detached = Core::detached(); assert!(detached.load_document(&inherited.to_string()).is_err());
        inherited["profiles"][0]["overrides"]["domain"] = json!(""); detached.load_document(&inherited.to_string()).unwrap();
        let mut edit = profile(&detached, ID); edit["overrides"]["domain"] = Value::Null;
        let before = detached.stored_document().unwrap(); assert!(detached.prepare_profile(ProfileChange::Update, &edit.to_string()).is_err());
        assert_eq!(detached.stored_document().unwrap(), before);
    }
    let mut wrong_schema = document(); wrong_schema["schema_version"] = json!(1);
    let mut detached = Core::detached(); assert!(detached.load_document(&wrong_schema.to_string()).is_err());
    wrong_schema["schema_version"] = json!(3); assert!(detached.load_document(&wrong_schema.to_string()).is_err());
}
