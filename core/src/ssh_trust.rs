use super::{ffi::HostKeyIdentity, Failure};
use russh::keys::ssh_key::{encoding::{Base64Reader, Base64Writer, Reader, Writer}, HashAlg, PublicKey};
use serde::{Deserialize, Serialize};
use std::collections::HashSet;

const CACHE_BYTES: usize = 1024 * 1024;
const RECORDS: usize = 1024;
const PUBLIC_KEY_BYTES: usize = 16 * 1024;

pub fn host_key_identity(host: String, port: u16, key: Vec<u8>) -> Result<HostKeyIdentity, Failure> {
    crate::model::validate_ssh_target(&host, port, "", "").map_err(|_| Failure::HostKey)?;
    if key.is_empty() || key.len() > PUBLIC_KEY_BYTES { return Err(Failure::HostKey); }
    let public = PublicKey::from_bytes(&key).map_err(|_| Failure::HostKey)?;
    super::public_key_algorithm(&public).map_err(|_| Failure::HostKey)?;
    if public.to_bytes().map_err(|_| Failure::HostKey)? != key { return Err(Failure::HostKey); }
    let host = host.parse::<std::net::IpAddr>().map(|address| address.to_string()).unwrap_or_else(|_| host.to_ascii_lowercase());
    Ok(HostKeyIdentity { host, port, algorithm: public.algorithm().to_string(), key,
        fingerprint: public.fingerprint(HashAlg::Sha256).to_string() })
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct StoredKey { host: String, port: u16, algorithm: String, key: String, fingerprint: String }
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Cache {
    schema: u32,
    #[serde(deserialize_with = "bounded_records")]
    records: Vec<StoredKey>,
}
fn bounded_records<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<Vec<StoredKey>, D::Error> {
    struct Records;
    impl<'de> serde::de::Visitor<'de> for Records {
        type Value = Vec<StoredKey>;
        fn expecting(&self, formatter: &mut std::fmt::Formatter) -> std::fmt::Result { formatter.write_str("at most 1024 host keys") }
        fn visit_seq<A: serde::de::SeqAccess<'de>>(self, mut sequence: A) -> Result<Self::Value, A::Error> {
            let mut records = Vec::new();
            while records.len() < RECORDS {
                match sequence.next_element()? { Some(record) => records.push(record), None => return Ok(records) }
            }
            if sequence.next_element::<serde::de::IgnoredAny>()?.is_some() { return Err(serde::de::Error::custom("too many host keys")); }
            Ok(records)
        }
    }
    deserializer.deserialize_seq(Records)
}
fn base64(bytes: &[u8]) -> Result<String, Failure> {
    let mut output = vec![0; bytes.len().div_ceil(3) * 4];
    let mut writer = Base64Writer::new(&mut output).map_err(|_| Failure::HostCache)?;
    writer.write(bytes).map_err(|_| Failure::HostCache)?;
    Ok(writer.finish().map_err(|_| Failure::HostCache)?.to_owned())
}
fn exact_record(record: &HostKeyIdentity) -> Result<(), Failure> {
    let checked = host_key_identity(record.host.clone(), record.port, record.key.clone()).map_err(|_| Failure::HostCache)?;
    if checked != *record { return Err(Failure::HostCache); }
    Ok(())
}
fn distinct(records: &[HostKeyIdentity]) -> Result<(), Failure> {
    let mut endpoints = HashSet::new();
    for record in records {
        if !endpoints.insert((&record.host, record.port, &record.algorithm)) { return Err(Failure::HostCache); }
    }
    Ok(())
}
pub fn decode_host_keys(bytes: &[u8]) -> Result<Vec<HostKeyIdentity>, Failure> {
    if bytes.len() > CACHE_BYTES { return Err(Failure::HostCache); }
    let cache: Cache = serde_json::from_slice(bytes).map_err(|_| Failure::HostCache)?;
    if cache.schema != 1 { return Err(Failure::HostCache); }
    let mut records = Vec::new();
    for stored in cache.records {
        if stored.key.len() > PUBLIC_KEY_BYTES.div_ceil(3) * 4 { return Err(Failure::HostCache); }
        let mut reader = Base64Reader::new(stored.key.as_bytes()).map_err(|_| Failure::HostCache)?;
        let mut key = vec![0; reader.remaining_len()];
        reader.read(&mut key).map_err(|_| Failure::HostCache)?;
        if base64(&key)? != stored.key { return Err(Failure::HostCache); }
        let record = HostKeyIdentity { host: stored.host, port: stored.port, algorithm: stored.algorithm, key, fingerprint: stored.fingerprint };
        exact_record(&record)?; records.push(record);
    }
    distinct(&records)?; Ok(records)
}
pub fn encode_host_keys(records: &Vec<HostKeyIdentity>) -> Result<String, Failure> {
    if records.len() > RECORDS { return Err(Failure::HostCache); }
    distinct(records)?;
    let mut sorted: Vec<_> = records.iter().collect();
    sorted.sort_by(|a, b| (&a.host, a.port, &a.algorithm).cmp(&(&b.host, b.port, &b.algorithm)));
    let mut stored = Vec::new();
    for record in sorted {
        exact_record(record)?;
        stored.push(StoredKey { host: record.host.clone(), port: record.port, algorithm: record.algorithm.clone(),
            key: base64(&record.key)?, fingerprint: record.fingerprint.clone() });
    }
    let bytes = serde_json::to_string(&Cache { schema: 1, records: stored }).map_err(|_| Failure::HostCache)?;
    if bytes.len() > CACHE_BYTES { return Err(Failure::HostCache); }
    Ok(bytes)
}
