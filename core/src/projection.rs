use crate::model::Document;

// Password-free in-memory channel only. XML disk I/O belongs to the C++ worker.
pub fn decode(bytes: &[u8]) -> Result<Document, String> {
    if bytes.len() > 16 * 1024 * 1024 { return Err("Profile projection exceeds 16 MiB".into()); }
    let document: Document = serde_json::from_slice(bytes).map_err(|_| "Invalid profile projection")?;
    document.validate()?;
    Ok(document)
}

pub fn encode(document: &Document) -> Result<String, String> {
    document.validate()?;
    let json = serde_json::to_string(document).map_err(|_| "Cannot encode profile projection")?;
    if json.len() > 16 * 1024 * 1024 { return Err("Profile projection exceeds 16 MiB".into()); }
    Ok(json)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Kind, Profile};
    #[test]
    fn projection_roundtrip_stable_ids_and_no_secrets() {
        let mut d = Document::default(); d.upsert(Profile::new("Server", Kind::Connection, None)).unwrap();
        let encoded = encode(&d).unwrap();
        let loaded = decode(encoded.as_bytes()).unwrap(); assert_eq!(d.profiles, loaded.profiles);
        assert!(!encoded.contains("password")); assert_eq!(encode(&loaded).unwrap(), encoded);
    }
    #[test]
    fn corrupt_unknown_version_and_secret_fields_are_rejected() {
        for text in ["{bad", "{\"schema_version\":99,\"profiles\":[]}",
                     "{\"schema_version\":2,\"profiles\":[],\"password\":\"SECRET\"}"] {
            let error = decode(text.as_bytes()).unwrap_err(); assert!(!error.contains("SECRET"));
        }
    }
}
