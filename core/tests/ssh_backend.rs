#![allow(non_snake_case)]
use remotemanager_core::{bridge::ffi, credentials::Secret, ssh::{self, Failure}, Core};
use russh::keys::ssh_key::{self, Algorithm, Cipher, Kdf, LineEnding, PrivateKey};
use ssh_key::encoding::{Base64Writer, Writer};
use std::{fs, path::PathBuf, process::Command};

struct OwnedKeys(PathBuf);
impl OwnedKeys {
    fn new() -> Self {
        let root = PathBuf::from(std::env::var("CARGO_TARGET_DIR").expect("Contained Cargo target required"));
        let path = root.join("ssh-test-keys").join(uuid::Uuid::new_v4().to_string());
        fs::create_dir_all(&path).unwrap(); Self(path)
    }
    fn generate(&self, kind: &str, bits: Option<u32>) -> PrivateKey {
        let path = self.0.join(format!("{kind}-{}", bits.unwrap_or(0)));
        let mut command = Command::new("ssh-keygen");
        command.args(["-q", "-t", kind, "-N", "", "-C", "", "-f"]).arg(&path);
        if let Some(bits) = bits { command.args(["-b", &bits.to_string()]); }
        let output = command.output().expect("Owned OpenSSH fixture generator required");
        assert!(output.status.success(), "Owned key generation failed");
        let bytes = fs::read(path).unwrap(); PrivateKey::from_openssh(&bytes).expect("Owned key must parse")
    }
}
impl Drop for OwnedKeys { fn drop(&mut self) { fs::remove_dir_all(&self.0).unwrap(); } }
fn secret(bytes: &[u8]) -> Secret { Secret::new(bytes.to_vec()) }
fn pem(bytes: &[u8]) -> Secret {
    let mut output = vec![0; bytes.len().div_ceil(3) * 4];
    let mut writer = Base64Writer::new(&mut output).unwrap(); writer.write(bytes).unwrap();
    let base64 = writer.finish().unwrap();
    let mut result = String::from("-----BEGIN OPENSSH PRIVATE KEY-----\n");
    for line in base64.as_bytes().chunks(70) { result.push_str(std::str::from_utf8(line).unwrap()); result.push('\n'); }
    result.push_str("-----END OPENSSH PRIVATE KEY-----\n"); secret(result.as_bytes())
}
fn take_string<'a>(bytes: &'a [u8], offset: &mut usize) -> &'a [u8] {
    let size = u32::from_be_bytes(bytes[*offset..*offset + 4].try_into().unwrap()) as usize;
    *offset += 4; let value = &bytes[*offset..*offset + size]; *offset += size; value
}
fn changed_kdf(key: &PrivateKey, rounds: u32, salt: &[u8]) -> Secret {
    let bytes = key.to_bytes().unwrap(); let mut offset = 15;
    take_string(&bytes, &mut offset); take_string(&bytes, &mut offset);
    let start = offset; take_string(&bytes, &mut offset);
    let mut changed = bytes[..start].to_vec(); changed.extend_from_slice(&((salt.len() + 8) as u32).to_be_bytes());
    changed.extend_from_slice(&(salt.len() as u32).to_be_bytes()); changed.extend_from_slice(salt);
    changed.extend_from_slice(&rounds.to_be_bytes()); changed.extend_from_slice(&bytes[offset..]); pem(&changed)
}

#[test]
fn sshAuthHandoffKeepsCoreRolesAndLogsSecretFree() {
    let mut core = Core::detached();
    core.load_document(r#"{"schema_version":2,"profiles":[]}"#).unwrap();
    let before = core.stored_document();
    let target = ffi::SshTarget { host: "localhost".into(), port: 22, username: "TEAM\\literal".into(),
        auth: "password".into(), key_path: String::new() };
    let info = core.open_temporary_ssh_session(&target, true).unwrap();
    let auth = ssh::password_auth(b"synthetic-session-password".to_vec()).unwrap();
    assert_eq!(format!("{auth:?}"), "Authentication([REDACTED])");
    assert_eq!(info.username, "TEAM\\literal"); assert_eq!(core.stored_document(), before);
    assert!(auth.into_password().unwrap().expose() == b"synthetic-session-password");
    let auth = ssh::private_key_auth("/owned/id_ed25519".into(), b"synthetic-passphrase".to_vec()).unwrap();
    assert_eq!(format!("{auth:?}"), "Authentication([REDACTED])");
    let (path, passphrase) = auth.into_key().unwrap(); assert_eq!(path, "/owned/id_ed25519");
    assert!(passphrase.expose() == b"synthetic-passphrase");
    assert!(ssh::password_auth(vec![b'x'; 4096]).is_ok());
    assert!(matches!(ssh::password_auth(vec![b'x'; 4097]), Err(Failure::AuthenticationBounds)));
    assert!(matches!(ssh::password_auth(vec![0xff]), Err(Failure::AuthenticationBounds)));
    assert!(matches!(ssh::private_key_auth("".into(), vec![]), Err(Failure::KeyPath)));
    assert!(matches!(ssh::private_key_auth("/owned/key\0".into(), vec![]), Err(Failure::KeyPath)));
    assert!(matches!(ssh::private_key_auth("/owned/key".into(), vec![b'x'; 4097]), Err(Failure::AuthenticationBounds)));
    assert!(ssh::password_auth(vec![]).unwrap().into_key().is_err());
    assert!(ssh::private_key_auth("/owned/key".into(), vec![]).unwrap().into_password().is_err());
    assert_eq!(core.stored_document(), before);
    let bridge = include_str!("../src/bridge.rs"); assert!(!bridge.contains("password_auth")); assert!(!bridge.contains("passphrase"));
}

#[test]
fn sshKeyFileReadIsBoundedAndCancellationReleasesMaterial() {
    let keys = OwnedKeys::new(); let owner = std::thread::current().id();
    let path = keys.0.join("bounded-key"); fs::write(&path, vec![b'x'; 1024 * 1024]).unwrap();
    let worker_path = path.to_str().unwrap().to_string();
    let bytes = std::thread::spawn(move || {
        assert_ne!(std::thread::current().id(), owner);
        ssh::read_private_key(&worker_path, || false).unwrap()
    }).join().unwrap();
    assert_eq!(bytes.expose().len(), 1024 * 1024); assert!(bytes.expose().iter().all(|byte| *byte == b'x'));
    fs::write(&path, vec![b'x'; 1024 * 1024 + 1]).unwrap();
    assert!(matches!(ssh::read_private_key(path.to_str().unwrap(), || false), Err(Failure::KeySize)));
    fs::write(&path, vec![b'x'; 64 * 1024]).unwrap();
    let checks = std::cell::Cell::new(0);
    let cancelled = ssh::read_private_key(path.to_str().unwrap(), || {
        checks.set(checks.get() + 1); checks.get() >= 4
    });
    assert!(matches!(cancelled, Err(Failure::Cancelled))); assert_eq!(checks.get(), 4);
    assert!(matches!(ssh::read_private_key(path.to_str().unwrap(), || true), Err(Failure::Cancelled)));
    assert!(matches!(ssh::read_private_key("", || false), Err(Failure::KeyPath)));
    assert!(matches!(ssh::read_private_key(keys.0.to_str().unwrap(), || false), Err(Failure::KeyFile)));
    let missing = keys.0.join("synthetic-secret-path");
    let error = ssh::read_private_key(missing.to_str().unwrap(), || false).err().unwrap().to_string();
    assert_eq!(error, "The SSH private-key file could not be read safely.");
    fs::write(&path, b"first").unwrap();
    let checks = std::cell::Cell::new(0);
    let changed = ssh::read_private_key(path.to_str().unwrap(), || {
        checks.set(checks.get() + 1);
        if checks.get() == 3 { fs::write(&path, b"changed-size").unwrap(); }
        false
    });
    assert!(matches!(changed, Err(Failure::KeyFile)));
    fs::write(&path, b"").unwrap(); assert!(ssh::read_private_key(path.to_str().unwrap(), || false).unwrap().expose().is_empty());
}

#[cfg(unix)]
#[test]
fn sshKeyFileNeverFollowsLinksOrBlocksOnSpecialFiles() {
    use std::os::unix::fs::symlink;
    let keys = OwnedKeys::new(); let target = keys.0.join("regular"); fs::write(&target, b"synthetic-key").unwrap();
    let link = keys.0.join("link"); symlink(&target, &link).unwrap();
    assert!(matches!(ssh::read_private_key(link.to_str().unwrap(), || false), Err(Failure::KeyFile)));
    let fifo = keys.0.join("fifo"); let name = std::ffi::CString::new(fifo.to_str().unwrap()).unwrap();
    assert_eq!(unsafe { libc::mkfifo(name.as_ptr(), 0o600) }, 0);
    assert!(matches!(ssh::read_private_key(fifo.to_str().unwrap(), || false), Err(Failure::KeyFile)));
    // Replace a regular path between validation and open. O_NONBLOCK and descriptor
    // validation must still reject the FIFO rather than wait for a writer.
    let race = keys.0.join("race"); fs::write(&race, b"synthetic-key").unwrap();
    let checks = std::cell::Cell::new(0);
    let result = ssh::read_private_key(race.to_str().unwrap(), || {
        checks.set(checks.get() + 1);
        if checks.get() == 2 { fs::remove_file(&race).unwrap(); fs::rename(&fifo, &race).unwrap(); }
        false
    });
    assert!(matches!(result, Err(Failure::KeyFile))); assert_eq!(checks.get(), 2);
}

// These synthetic components exercise decoder bounds, not RSA mathematics or authentication.
fn rsa_bounds_fixture(bits: usize, oversized: Option<usize>) -> Secret {
    use ssh_key::{encoding::Mpint, private::{RsaKeypair, RsaPrivateKey}, public::RsaPublicKey};
    let mut modulus = vec![0; bits.div_ceil(8)]; modulus[0] = 1 << ((bits - 1) % 8); *modulus.last_mut().unwrap() |= 1;
    let component = |index| Mpint::from_positive_bytes(&if oversized == Some(index) { vec![1; modulus.len() + 1] } else { vec![3] });
    let public = RsaPublicKey::new(component(0), Mpint::from_positive_bytes(&modulus)).unwrap();
    let private = RsaPrivateKey::new(component(1), component(2), component(3), component(4)).unwrap();
    let pair = RsaKeypair::new(public, private).unwrap();
    let key = PrivateKey::new(pair.into(), "").unwrap(); let encoded = key.to_openssh(LineEnding::LF).unwrap(); secret(encoded.as_bytes())
}

#[test]
fn sshRsaExactBitAndComponentBounds() {
    for bits in [2048, 8192] { assert!(ssh::decode_private_key(rsa_bounds_fixture(bits, None), &secret(b"")).is_ok()); }
    for bits in [2041, 2047, 8193] {
        assert!(matches!(ssh::decode_private_key(rsa_bounds_fixture(bits, None), &secret(b"")), Err(Failure::KeyAlgorithm)));
    }
    for component in 0..5 {
        assert!(matches!(ssh::decode_private_key(rsa_bounds_fixture(2048, Some(component)), &secret(b"")), Err(Failure::KeyAlgorithm)));
    }
}

#[test]
fn sshBcryptMaximumBoundsDecryptAndEmptySaltRejects() {
    let key = PrivateKey::from(ssh_key::private::Ed25519Keypair::from_seed(&[7; 32]));
    let encrypted = key.encrypt_with(Cipher::Aes256Ctr, Kdf::Bcrypt { salt: vec![9; 64], rounds: 64 }, 0x12345678, b"synthetic-key-passphrase").unwrap();
    let encoded = encrypted.to_openssh(LineEnding::LF).unwrap();
    let decoded = ssh::decode_private_key(secret(encoded.as_bytes()), &secret(b"synthetic-key-passphrase")).unwrap();
    assert!(decoded.public_key() == key.public_key());
    assert!(matches!(ssh::decode_private_key(changed_kdf(&encrypted, 1, &[]), &secret(b"wrong")), Err(Failure::KeyKdf)));
}

#[test]
fn sshKeyFormatsAndKdfBoundsRejectSafely() {
    let keys = OwnedKeys::new();
    for (kind, bits) in [("ed25519", None), ("rsa", Some(2048)), ("ecdsa", Some(256)), ("ecdsa", Some(384)), ("ecdsa", Some(521))] {
        let key = keys.generate(kind, bits);
        let plain = key.to_openssh(LineEnding::LF).unwrap();
        let decoded = ssh::decode_private_key(secret(plain.as_bytes()), &secret(b"unused-passphrase")).unwrap();
        assert!(decoded.public_key() == key.public_key()); assert!(!decoded.is_encrypted());
        let encrypted = key.encrypt_with(Cipher::Aes256Ctr, Kdf::Bcrypt { salt: vec![9; 16], rounds: 1 }, 0x12345678, b"synthetic-key-passphrase").unwrap();
        let encoded = encrypted.to_openssh(LineEnding::LF).unwrap();
        let decoded = ssh::decode_private_key(secret(encoded.as_bytes()), &secret(b"synthetic-key-passphrase")).unwrap();
        assert!(decoded.public_key() == key.public_key()); assert!(!decoded.is_encrypted());
        assert!(matches!(ssh::decode_private_key(secret(encoded.as_bytes()), &secret(b"wrong")), Err(Failure::KeyPassphrase)));
        for rounds in [0, 65, u32::MAX] {
            assert!(matches!(ssh::decode_private_key(changed_kdf(&encrypted, rounds, &[9; 16]), &secret(b"wrong")), Err(Failure::KeyKdf)));
        }
        assert!(matches!(ssh::decode_private_key(changed_kdf(&encrypted, 1, &[9; 65]), &secret(b"wrong")), Err(Failure::KeyKdf)));
    }
    let weak = keys.generate("rsa", Some(1024)); let encoded = weak.to_openssh(LineEnding::LF).unwrap();
    assert!(matches!(ssh::decode_private_key(secret(encoded.as_bytes()), &secret(b"")), Err(Failure::KeyAlgorithm)));
    let ed25519 = PrivateKey::from(ssh_key::private::Ed25519Keypair::from_seed(&[7; 32]));
    assert_eq!(ed25519.algorithm(), Algorithm::Ed25519);
    for bytes in [b"".as_slice(), b"not a key", b"-----BEGIN RSA PRIVATE KEY-----\ninvalid", b"PuTTY-User-Key-File-3: ssh-ed25519"] {
        assert!(matches!(ssh::decode_private_key(secret(bytes), &secret(b"")), Err(Failure::KeyFormat)));
    }
    assert!(matches!(ssh::decode_private_key(Secret::new(vec![b'x'; 1024 * 1024 + 1]), &secret(b"")), Err(Failure::KeySize)));
    assert!(matches!(ssh::decode_private_key(secret(b"not a key"), &Secret::new(vec![0xff])), Err(Failure::AuthenticationBounds)));
}
