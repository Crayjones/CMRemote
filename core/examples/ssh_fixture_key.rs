use russh::keys::ssh_key::{private::Ed25519Keypair, Cipher, Kdf, LineEnding, PrivateKey};
use std::{fs::OpenOptions, io::Write, path::PathBuf};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let root = PathBuf::from(std::env::var("RM_BUILD_DIR")?).canonicalize()?;
    let directory = PathBuf::from(std::env::var("RM_SSH_KEY_DIRECTORY")?).canonicalize()?;
    if !directory.starts_with(root.join("test-data")) { return Err("Key fixture must be contained in test-data".into()); }
    let passphrase = std::env::var("RM_SSH_TEST_PASSPHRASE")?;
    if passphrase.is_empty() || passphrase.len() > 4096 { return Err("Invalid synthetic fixture passphrase".into()); }
    let mut seed = [0; 32]; seed[..16].copy_from_slice(uuid::Uuid::new_v4().as_bytes()); seed[16..].copy_from_slice(uuid::Uuid::new_v4().as_bytes());
    let key = PrivateKey::from(Ed25519Keypair::from_seed(&seed));
    let encrypted = key.encrypt_with(Cipher::Aes256Ctr, Kdf::Bcrypt { salt: uuid::Uuid::new_v4().as_bytes().to_vec(), rounds: 4 }, 0x573a0129, passphrase.as_bytes())?;
    for (name, bytes) in [("encrypted-key", encrypted.to_openssh(LineEnding::LF)?.as_bytes().to_vec()),
        ("authorized-key", key.public_key().to_openssh()?.into_bytes())] {
        let mut options = OpenOptions::new(); options.write(true).create_new(true);
        #[cfg(unix)] { use std::os::unix::fs::OpenOptionsExt; options.mode(0o600); }
        options.open(directory.join(name))?.write_all(&bytes)?;
    }
    Ok(())
}
