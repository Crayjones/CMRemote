use remotemanager_core::{Core, model::Document};
use serde_json::{json, Value};

const ID: &str = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
fn document(protocol: &str, unsupported: Value) -> String {
    json!({"schema_version":2,"profiles":[{"id":ID,"name":"native","kind":"connection","parent":null,
        "overrides":{"protocol":protocol,"host":"localhost","port":3389,"username":""},
        "native":{"unsupported":unsupported}}]}).to_string()
}

#[test]
#[allow(non_snake_case)]
fn unsupportedNativeProjectionCannotLaunchEvenAsMock() {
    for (protocol, reasons) in [("unsupported", json!(["protocol"])), ("rdp", json!(["resolution"])),
                                ("rdp", json!(["gateway", "authentication"])),
                                ("ssh", json!(["command"])), ("ssh", json!(["opaque"]))] {
        let mut core = Core::detached(); core.load_document(&document(protocol, reasons)).unwrap();
        let before = core.stored_document().unwrap();
        assert!(core.open_session(ID, false).is_err()); assert!(core.open_session(ID, true).is_err());
        assert_eq!(core.session_count(), 0); assert_eq!(core.stored_document().unwrap(), before);
        let snapshot: Value = serde_json::from_str(&core.snapshot().unwrap()).unwrap();
        assert!(!snapshot["profiles"][0]["effective"]["unsupported"].as_array().unwrap().is_empty());
    }
    let mut core = Core::detached(); core.load_document(&document("rdp", json!([]))).unwrap();
    let session = core.open_session(ID, true).unwrap(); core.close_session(&session.id);
    core.load_document(&document("ssh", json!([]))).unwrap();
    let session = core.open_session(ID, true).unwrap();
    assert_eq!(session.protocol, "ssh");
    assert!(session.mock && session.keyboard && session.resize);
    assert!(!session.graphical && !session.mouse && !session.clipboard);
    core.close_session(&session.id); assert_eq!(core.session_count(), 0);
}

#[test]
fn native_capability_is_bounded_typed_and_not_user_editable() {
    for reasons in [json!(["arbitrary input"]), json!(["gateway", "gateway"]), json!("gateway")] {
        assert!(serde_json::from_str::<Document>(&document("rdp", reasons)).and_then(|d| d.validate().map_err(serde::de::Error::custom)).is_err());
    }
    let mut core = Core::detached(); core.load_document(&document("rdp", json!(["gateway"]))).unwrap();
    let mut profile: Value = serde_json::from_str::<Value>(&core.stored_document().unwrap()).unwrap()["profiles"][0].clone();
    profile["native"]["unsupported"] = json!([]); assert!(core.preview_profile(&profile.to_string()).is_err());
    profile.as_object_mut().unwrap().remove("native"); assert!(core.preview_profile(&profile.to_string()).is_err());
}
