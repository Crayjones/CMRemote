use remotemanager_core::ssh::{self, ffi::HostKeyIdentity, Failure};
use russh::keys::ssh_key::{self, PrivateKey};

fn key(seed: u8, host: &str, port: u16) -> HostKeyIdentity {
    let private = PrivateKey::from(ssh_key::private::Ed25519Keypair::from_seed(&[seed; 32]));
    ssh::host_key_identity(host.into(), port, private.public_key().to_bytes().unwrap()).unwrap()
}

#[test]
fn host_identity_uses_configured_endpoint_and_exact_public_key() {
    let first = key(7, "OWNED.Example", 22);
    assert_eq!(first.host, "owned.example");
    assert_eq!(first.algorithm, "ssh-ed25519");
    assert!(first.fingerprint.starts_with("SHA256:"));
    assert_eq!(key(7, "owned.example", 2222).key, first.key);
    assert_ne!(key(8, "owned.example", 22).fingerprint, first.fingerprint);
    assert_eq!(key(7, "2001:0DB8:0:0::1", 22).host, "2001:db8::1");
    let mut trailing = first.key.clone(); trailing.push(0);
    assert_eq!(ssh::host_key_identity(first.host.clone(), 22, trailing).unwrap_err(), Failure::HostKey);
    for bytes in [vec![], vec![0; 16385], vec![1, 2, 3]] {
        assert_eq!(ssh::host_key_identity("owned.example".into(), 22, bytes).unwrap_err(), Failure::HostKey);
    }
    assert!(ssh::host_key_identity("owned\nhost".into(), 22, first.key.clone()).is_err());
    assert!(ssh::host_key_identity(first.host, 0, first.key).is_err());
}

#[test]
fn host_rsa_significant_bits_and_exponent_are_bounded() {
    use ssh_key::{encoding::Mpint, public::RsaPublicKey, PublicKey};
    for bits in [1024usize, 2047, 2048, 8192, 8193] {
        let mut modulus = vec![0; bits.div_ceil(8)]; modulus[0] = 1 << ((bits - 1) % 8); *modulus.last_mut().unwrap() |= 1;
        // Synthetic grammar/bounds fixture, not a mathematically valid RSA key or authentication proof.
        let public = PublicKey::new(RsaPublicKey::new(Mpint::from_positive_bytes(&[3]), Mpint::from_positive_bytes(&modulus)).unwrap().into(), "");
        let result = ssh::host_key_identity("owned.example".into(), 22, public.to_bytes().unwrap());
        if (2048..=8192).contains(&bits) { assert_eq!(result.unwrap().algorithm, "ssh-rsa"); }
        else { assert_eq!(result.unwrap_err(), Failure::HostKey); }
    }
    let public = PublicKey::new(RsaPublicKey::new(Mpint::from_positive_bytes(&vec![1; 257]), Mpint::from_positive_bytes(&vec![0xff; 256])).unwrap().into(), "");
    assert_eq!(ssh::host_key_identity("owned.example".into(), 22, public.to_bytes().unwrap()).unwrap_err(), Failure::HostKey);
}

#[test]
fn host_cache_schema_is_strict_bounded_and_fingerprint_checked() {
    let record = key(7, "owned.example", 22);
    let encoded = ssh::encode_host_keys(&vec![record.clone()]).unwrap();
    assert_eq!(ssh::decode_host_keys(encoded.as_bytes()).unwrap(), vec![record]);
    let value: serde_json::Value = serde_json::from_str(&encoded).unwrap();
    let mut bad_fingerprint = value.clone(); bad_fingerprint["records"][0]["fingerprint"] = "SHA256:invented".into();
    let mut bad_algorithm = value.clone(); bad_algorithm["records"][0]["algorithm"] = "ssh-rsa".into();
    let mut noncanonical = value.clone(); noncanonical["records"][0]["host"] = "OWNED.example".into();
    let mut duplicate = value.clone(); duplicate["records"].as_array_mut().unwrap().push(value["records"][0].clone());
    let mut extra = value.clone(); extra["unrecognized"] = true.into();
    for invalid in [bad_fingerprint, bad_algorithm, noncanonical, duplicate, extra] {
        assert_eq!(ssh::decode_host_keys(invalid.to_string().as_bytes()).unwrap_err(), Failure::HostCache);
    }
    for invalid in [r#"{"schema":1,"schema":1,"records":[]}"#, r#"{"schema":2,"records":[]}"#,
                    r#"{"schema":1.0,"records":[]}"#, r#"{"schema":1,"records":[]} false"#] {
        assert_eq!(ssh::decode_host_keys(invalid.as_bytes()).unwrap_err(), Failure::HostCache);
    }
    assert_eq!(ssh::decode_host_keys(&vec![b' '; 1024 * 1024 + 1]).unwrap_err(), Failure::HostCache);
    let records: Vec<_> = (0..1024).map(|i| key(7, &format!("owned-{i}.example"), 22)).collect();
    let maximum = ssh::encode_host_keys(&records).unwrap();
    assert_eq!(ssh::decode_host_keys(maximum.as_bytes()).unwrap().len(), 1024);
    let mut excessive = records; excessive.push(key(7, "too-many.example", 22));
    assert_eq!(ssh::encode_host_keys(&excessive).unwrap_err(), Failure::HostCache);
    let mut oversized: serde_json::Value = serde_json::from_str(&maximum).unwrap();
    oversized["records"].as_array_mut().unwrap().push(value["records"][0].clone());
    assert_eq!(ssh::decode_host_keys(oversized.to_string().as_bytes()).unwrap_err(), Failure::HostCache);
}
