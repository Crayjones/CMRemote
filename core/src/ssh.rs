use crate::credentials::Secret;
use russh::keys::ssh_key::{Algorithm, EcdsaCurve, Kdf, PrivateKey, PublicKey};

#[path = "ssh_stream.rs"]
pub mod stream;
#[path = "ssh_transport.rs"]
pub mod transport;
#[path = "ssh_trust.rs"]
mod trust;
pub use trust::{decode_host_keys, encode_host_keys, host_key_identity};
pub use transport::{Backend, BackendControl};

#[cxx::bridge(namespace = "rm::ssh")]
pub mod ffi {
    struct BackendTarget {
        host: String,
        port: u16,
        username: String,
        auth: String,
        key_path: String,
        rows: u16,
        columns: u16,
        /// Empty opens an interactive shell; otherwise the channel runs this command
        /// without a PTY (CMRemote's SCP upload).
        command: String,
    }
    #[derive(Clone, Debug, PartialEq, Eq)]
    struct HostKeyIdentity {
        host: String,
        port: u16,
        algorithm: String,
        key: Vec<u8>,
        fingerprint: String,
    }
    struct OutputPacket { bytes: Vec<u8> }
    struct BackendEvent {
        phase: u8,
        error: String,
        host_pending: bool,
        host_request: String,
        host_key: HostKeyIdentity,
        packets: Vec<OutputPacket>,
        ended: bool,
    }
    unsafe extern "C++" {
        include!("SshEventSink.h");
        type EventSink;
        fn wake(self: &EventSink, generation: u64);
    }
    extern "Rust" {
        type Backend;
        type BackendControl;
        fn new_backend(target: BackendTarget, auth: Box<Authentication>, sink: SharedPtr<EventSink>, generation: u64) -> Result<Box<Backend>>;
        fn control(self: &Backend) -> Box<BackendControl>;
        fn run_backend(backend: Box<Backend>);
        fn input(self: &BackendControl, bytes: &[u8]) -> Result<()>;
        fn resize(self: &BackendControl, rows: u16, columns: u16) -> Result<()>;
        fn stop(self: &BackendControl);
        fn resolved(self: &BackendControl, addresses: Vec<String>) -> bool;
        fn host_answer(self: &BackendControl, request: &str, allowed: bool) -> bool;
        fn poll(self: &BackendControl) -> BackendEvent;
        fn host_key_identity(host: String, port: u16, key: Vec<u8>) -> Result<HostKeyIdentity>;
        fn decode_host_keys(bytes: &[u8]) -> Result<Vec<HostKeyIdentity>>;
        fn encode_host_keys(records: &Vec<HostKeyIdentity>) -> Result<String>;
        type Authentication;
        fn password_auth(password: Vec<u8>) -> Result<Box<Authentication>>;
        fn private_key_auth(path: String, passphrase: Vec<u8>) -> Result<Box<Authentication>>;
    }
}

// EventSink owns an immutable callback that only queues to a stable Qt delivery
// object. No GUI access or mutation occurs on the calling worker thread.
unsafe impl Send for ffi::EventSink {}
unsafe impl Sync for ffi::EventSink {}
pub fn new_backend(target: ffi::BackendTarget, auth: Box<Authentication>, sink: cxx::SharedPtr<ffi::EventSink>, generation: u64) -> Result<Box<Backend>, Failure> {
    if sink.is_null() { return Err(Failure::Runtime); }
    let (_, backend) = transport::create(target, auth, move || sink.wake(generation))?;
    Ok(Box::new(backend))
}
pub fn run_backend(backend: Box<Backend>) { backend.run(); }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Failure { AuthenticationBounds, AuthenticationMethod, KeyPath, KeyFile, KeySize, KeyFormat, KeyKdf, KeyAlgorithm, KeyPassphrase, Cancelled, HostKey, HostCache, StreamBounds, StreamClosed, InputOverload, TerminalSize, Target, Resolve, Connect, Handshake, HostRejected, AuthenticationRejected, Channel, Pty, Shell, Deadline, Runtime }
impl std::fmt::Display for Failure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::AuthenticationBounds => "SSH authentication must be at most 4096 UTF-8 bytes.",
            Self::AuthenticationMethod => "SSH authentication method does not match the session.",
            Self::KeyPath => "Choose a valid OpenSSH private-key path.",
            Self::KeyFile => "The SSH private-key file could not be read safely.",
            Self::Cancelled => "SSH connection cancelled.",
            Self::HostKey => "The SSH server key is unsupported or malformed.",
            Self::HostCache => "The SSH host-key cache is malformed or exceeds supported limits.",
            Self::StreamBounds => "SSH stream packets must contain 1–32768 bytes.",
            Self::StreamClosed => "The SSH session no longer accepts input or output.",
            Self::InputOverload => "The SSH input queue is full. Input was not accepted.",
            Self::TerminalSize => "SSH terminals require 1–256 rows and 1–512 columns.",
            Self::KeySize => "SSH private-key files must be at most 1 MiB.",
            Self::KeyFormat => "The file is not a supported OpenSSH private key.",
            Self::KeyKdf => "OpenSSH bcrypt parameters exceed supported limits.",
            Self::KeyAlgorithm => "SSH requires Ed25519, ECDSA, or 2048–8192-bit RSA keys.",
            Self::KeyPassphrase => "The SSH private key could not be decrypted with this passphrase.",
            Self::Target => "The SSH target is invalid or exceeds supported limits.",
            Self::Resolve => "The SSH hostname could not be resolved.",
            Self::Connect => "The SSH server could not be reached.",
            Self::Handshake => "The SSH handshake failed.",
            Self::HostRejected => "The SSH server key was not approved.",
            Self::AuthenticationRejected => "The SSH server did not accept the requested authentication.",
            Self::Channel => "The SSH terminal channel failed.",
            Self::Pty => "The SSH server refused the terminal request.",
            Self::Shell => "The SSH server refused the interactive shell request.",
            Self::Deadline => "The SSH connection stage timed out.",
            Self::Runtime => "The SSH worker could not start.",
        })
    }
}
impl std::error::Error for Failure {}

enum AuthenticationKind { Password(Secret), PrivateKey { path: String, passphrase: Secret } }
pub struct Authentication { kind: AuthenticationKind }
impl std::fmt::Debug for Authentication {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str("Authentication([REDACTED])") }
}
impl Authentication {
    pub fn into_password(self) -> Result<Secret, Failure> {
        match self.kind { AuthenticationKind::Password(password) => Ok(password), _ => Err(Failure::AuthenticationMethod) }
    }
    pub fn into_key(self) -> Result<(String, Secret), Failure> {
        match self.kind { AuthenticationKind::PrivateKey { path, passphrase } => Ok((path, passphrase)), _ => Err(Failure::AuthenticationMethod) }
    }
}
fn authentication_bounds(secret: &Secret) -> Result<(), Failure> {
    if secret.expose().len() > 4096 || std::str::from_utf8(secret.expose()).is_err() { Err(Failure::AuthenticationBounds) } else { Ok(()) }
}
pub fn password_auth(password: Vec<u8>) -> Result<Box<Authentication>, Failure> {
    let password = Secret::new(password); authentication_bounds(&password)?;
    Ok(Box::new(Authentication { kind: AuthenticationKind::Password(password) }))
}
pub fn private_key_auth(path: String, passphrase: Vec<u8>) -> Result<Box<Authentication>, Failure> {
    let passphrase = Secret::new(passphrase); authentication_bounds(&passphrase)?;
    if path.is_empty() || path.len() > 4096 || path.chars().any(char::is_control) { return Err(Failure::KeyPath); }
    Ok(Box::new(Authentication { kind: AuthenticationKind::PrivateKey { path, passphrase } }))
}
fn public_key_algorithm(key: &PublicKey) -> Result<usize, Failure> {
    match key.algorithm() {
        Algorithm::Ed25519 | Algorithm::Ecdsa { curve: EcdsaCurve::NistP256 | EcdsaCurve::NistP384 | EcdsaCurve::NistP521 } => Ok(0),
        Algorithm::Rsa { .. } => {
            let public = key.key_data().rsa().ok_or(Failure::KeyAlgorithm)?;
            let modulus = public.n().as_positive_bytes().ok_or(Failure::KeyAlgorithm)?;
            let first = modulus.first().copied().ok_or(Failure::KeyAlgorithm)?;
            // ssh-key reports octet length * 8, which rounds sub-2048-bit values up.
            let bits = modulus.len() * 8 - first.leading_zeros() as usize;
            if !(2048..=8192).contains(&bits) { return Err(Failure::KeyAlgorithm); }
            let limit = bits.div_ceil(8);
            if !bounded_component(public.e(), limit) { return Err(Failure::KeyAlgorithm); }
            Ok(limit)
        }
        _ => Err(Failure::KeyAlgorithm),
    }
}
fn bounded_component(value: &russh::keys::ssh_encoding::Mpint, limit: usize) -> bool {
    value.as_positive_bytes().is_some_and(|bytes| !bytes.is_empty() && bytes.len() <= limit)
}
fn key_algorithm(key: &PrivateKey) -> Result<(), Failure> {
    let limit = public_key_algorithm(key.public_key())?;
    if let Some(pair) = key.key_data().rsa() {
        let private = pair.private();
        for value in [private.d(), private.iqmp(), private.p(), private.q()] {
            if !bounded_component(value, limit) { return Err(Failure::KeyAlgorithm); }
        }
    }
    Ok(())
}

const KEY_BYTES: usize = 1024 * 1024;

// Backend-only I/O. Nonblocking/no-follow open plus descriptor validation closes
// the regular-file-to-FIFO race; cancellation is checked between bounded reads.
pub fn read_private_key(path: &str, cancelled: impl Fn() -> bool) -> Result<Secret, Failure> {
    use std::{fs, io::Read};
    if cancelled() { return Err(Failure::Cancelled); }
    if path.is_empty() || path.len() > 4096 || path.chars().any(char::is_control) { return Err(Failure::KeyPath); }
    let prior = fs::symlink_metadata(path).map_err(|_| Failure::KeyFile)?;
    if !prior.is_file() { return Err(Failure::KeyFile); }
    if prior.len() > KEY_BYTES as u64 { return Err(Failure::KeySize); }
    if cancelled() { return Err(Failure::Cancelled); }
    let mut options = fs::OpenOptions::new(); options.read(true);
    #[cfg(unix)] {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NONBLOCK | libc::O_NOFOLLOW | libc::O_CLOEXEC);
    }
    #[cfg(windows)] {
        use std::os::windows::fs::OpenOptionsExt;
        const FILE_FLAG_OPEN_REPARSE_POINT: u32 = 0x00200000;
        options.custom_flags(FILE_FLAG_OPEN_REPARSE_POINT);
    }
    let mut file = options.open(path).map_err(|_| Failure::KeyFile)?;
    let opened = file.metadata().map_err(|_| Failure::KeyFile)?;
    if !opened.is_file() { return Err(Failure::KeyFile); }
    #[cfg(unix)] {
        use std::os::unix::fs::MetadataExt;
        if opened.dev() != prior.dev() || opened.ino() != prior.ino() { return Err(Failure::KeyFile); }
    }
    #[cfg(windows)] {
        use std::os::windows::fs::MetadataExt;
        const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
        if opened.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0 { return Err(Failure::KeyFile); }
    }
    if opened.len() > KEY_BYTES as u64 { return Err(Failure::KeySize); }
    if opened.len() != prior.len() || opened.modified().ok() != prior.modified().ok() { return Err(Failure::KeyFile); }
    let length = opened.len() as usize; let mut bytes = Secret::new(vec![0; length + 1]); let mut used = 0;
    loop {
        if cancelled() { return Err(Failure::Cancelled); }
        let end = (used + 16 * 1024).min(length + 1);
        let read = file.read(&mut bytes.expose_mut()[used..end]).map_err(|_| Failure::KeyFile)?;
        if read == 0 { break; }
        used += read;
        if used > KEY_BYTES { return Err(Failure::KeySize); }
        if used > length { return Err(Failure::KeyFile); }
    }
    if cancelled() { return Err(Failure::Cancelled); }
    let after = file.metadata().map_err(|_| Failure::KeyFile)?;
    if used != length || after.len() != opened.len() || after.modified().ok() != opened.modified().ok() { return Err(Failure::KeyFile); }
    bytes.truncate(used); Ok(bytes)
}

// Backend-only decoder: callers do bounded regular-file I/O off the GUI first.
pub fn decode_private_key(bytes: Secret, passphrase: &Secret) -> Result<PrivateKey, Failure> {
    authentication_bounds(passphrase)?;
    if bytes.expose().len() > KEY_BYTES { return Err(Failure::KeySize); }
    let key = PrivateKey::from_openssh(bytes.expose()).map_err(|_| Failure::KeyFormat)?;
    drop(bytes);
    match key.kdf() {
        Kdf::None if !key.is_encrypted() => {},
        Kdf::Bcrypt { salt, rounds } if key.is_encrypted() && !salt.is_empty() && salt.len() <= 64 && (1..=64).contains(rounds) => {},
        _ => return Err(Failure::KeyKdf),
    }
    key_algorithm(&key)?;
    let key = if key.is_encrypted() { key.decrypt(passphrase.expose()).map_err(|_| Failure::KeyPassphrase)? } else { key };
    key_algorithm(&key)?; Ok(key)
}
