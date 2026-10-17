#![allow(non_snake_case)]
use remotemanager_core::ssh::{self, ffi::BackendTarget, transport::{self, Phase}};
use russh::{server, Channel, ChannelId, Pty};
use russh::keys::ssh_key::{PrivateKey, private::Ed25519Keypair};
use std::{sync::{Arc, atomic::{AtomicUsize, Ordering}}, time::Duration};
use tokio::{net::TcpListener, time::{sleep, timeout}};

#[derive(Clone)]
struct Counts { auth: Arc<AtomicUsize>, pty: Arc<AtomicUsize>, shell: Arc<AtomicUsize>, input: Arc<AtomicUsize> }
impl Counts {
    fn new() -> Self { Self { auth: Arc::new(AtomicUsize::new(0)), pty: Arc::new(AtomicUsize::new(0)), shell: Arc::new(AtomicUsize::new(0)), input: Arc::new(AtomicUsize::new(0)) } }
}
struct OwnedServer { counts: Counts, pty: bool, shell: bool, auth_gate: Option<Arc<tokio::sync::Notify>>, flood: bool }
impl server::Handler for OwnedServer {
    type Error = russh::Error;
    async fn auth_password(&mut self, user: &str, password: &str) -> Result<server::Auth, Self::Error> {
        self.counts.auth.fetch_add(1, Ordering::SeqCst);
        if let Some(gate) = &self.auth_gate { gate.notified().await; }
        Ok(if user == "TEAM\\literal" && password == "synthetic-loopback-password" { server::Auth::Accept } else { server::Auth::reject() })
    }
    async fn auth_publickey(&mut self, user: &str, key: &russh::keys::PublicKey) -> Result<server::Auth, Self::Error> {
        self.counts.auth.fetch_add(1, Ordering::SeqCst);
        let expected = PrivateKey::from(Ed25519Keypair::from_seed(&[83; 32]));
        Ok(if user == "TEAM\\literal" && key == expected.public_key() { server::Auth::Accept } else { server::Auth::reject() })
    }
    async fn channel_open_session(&mut self, _: Channel<server::Msg>, reply: server::ChannelOpenHandle, _: &mut server::Session) -> Result<(), Self::Error> {
        reply.accept().await; Ok(())
    }
    async fn pty_request(&mut self, channel: ChannelId, term: &str, columns: u32, rows: u32, _: u32, _: u32, _: &[(Pty, u32)], session: &mut server::Session) -> Result<(), Self::Error> {
        self.counts.pty.fetch_add(1, Ordering::SeqCst);
        assert_eq!(term, "xterm-256color"); assert_eq!((rows, columns), (24, 80));
        if self.pty { session.channel_success(channel)?; } else { session.channel_failure(channel)?; } Ok(())
    }
    async fn shell_request(&mut self, channel: ChannelId, session: &mut server::Session) -> Result<(), Self::Error> {
        self.counts.shell.fetch_add(1, Ordering::SeqCst);
        if self.shell { session.channel_success(channel)?; session.data(channel, b"owned ready\r\n".to_vec())?; }
        else { session.channel_failure(channel)?; }
        if self.shell && self.flood {
            for _ in 0..128 { session.data(channel, vec![b'x'; 32768])?; }
        }
        Ok(())
    }
    async fn data(&mut self, channel: ChannelId, data: &[u8], session: &mut server::Session) -> Result<(), Self::Error> {
        self.counts.input.fetch_add(data.len(), Ordering::SeqCst);
        session.data(channel, data.to_vec())?; Ok(())
    }
}
async fn server(pty: bool, shell: bool) -> (u16, Counts, tokio::task::JoinHandle<()>) { server_with(pty, shell, None, false).await }
async fn server_with(pty: bool, shell: bool, auth_gate: Option<Arc<tokio::sync::Notify>>, flood: bool) -> (u16, Counts, tokio::task::JoinHandle<()>) {
    let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)).await.unwrap();
    let port = listener.local_addr().unwrap().port(); let counts = Counts::new(); let observed = counts.clone();
    let key = PrivateKey::new(Ed25519Keypair::from_seed(&[71; 32]).into(), "").unwrap();
    let config = Arc::new(server::Config { keys: vec![key], auth_rejection_time: Duration::ZERO, ..Default::default() });
    let task = tokio::spawn(async move {
        let (socket, _) = listener.accept().await.unwrap();
        let session = server::run_stream(config, socket, OwnedServer { counts: observed, pty, shell, auth_gate, flood }).await.unwrap();
        let _ = session.await;
    });
    (port, counts, task)
}
fn target(port: u16) -> BackendTarget {
    BackendTarget { host: "configured.invalid".into(), port, username: "TEAM\\literal".into(), auth: "password".into(), key_path: String::new(), rows: 24, columns: 80 , command: String::new()}
}
fn worker(port: u16) -> (transport::BackendControl, std::thread::JoinHandle<()>) {
    let auth = ssh::password_auth(b"synthetic-loopback-password".to_vec()).unwrap();
    let (control, backend) = transport::create(target(port), auth, || {}).unwrap();
    assert!(control.resolved(vec!["127.0.0.1".into()]));
    let thread = std::thread::spawn(move || backend.run()); (control, thread)
}
async fn challenge(control: &transport::BackendControl) -> transport::HostChallenge {
    timeout(Duration::from_secs(5), async {
        loop { if let Some(challenge) = control.drain().host { return challenge; } sleep(Duration::from_millis(2)).await; }
    }).await.expect("Owned host-key challenge")
}
async fn phase(control: &transport::BackendControl, expected: Phase) {
    timeout(Duration::from_secs(5), async {
        loop { if control.drain().phase == expected { return; } sleep(Duration::from_millis(2)).await; }
    }).await.expect("Expected owned transport phase")
}

#[tokio::test(flavor = "current_thread")]
async fn sshHostApprovalPrecedesPasswordOnOwnedLoopback() {
    let (port, counts, server) = server(true, true).await;
    let (control, thread) = worker(port); let key = challenge(&control).await;
    assert_eq!(key.identity.host, "configured.invalid"); assert_eq!(key.identity.port, port);
    assert_eq!(counts.auth.load(Ordering::SeqCst), 0);
    assert!(!control.host_answer("stale-request", true));
    sleep(Duration::from_millis(20)).await; assert_eq!(counts.auth.load(Ordering::SeqCst), 0);
    assert!(control.host_answer(&key.request, true)); assert!(!control.host_answer(&key.request, true));
    phase(&control, Phase::Connected).await;
    assert_eq!(counts.auth.load(Ordering::SeqCst), 1);
    assert_eq!(counts.pty.load(Ordering::SeqCst), 1); assert_eq!(counts.shell.load(Ordering::SeqCst), 1);
    let input = "owned Unicode λ界\r".as_bytes(); control.input(input).unwrap();
    let mut received = Vec::new();
    timeout(Duration::from_secs(5), async {
        loop {
            received.extend(control.drain().stream.packets.into_iter().flatten());
            if received.windows(input.len()).any(|window| window == input) { break; }
            sleep(Duration::from_millis(2)).await;
        }
    }).await.unwrap();
    assert_eq!(counts.input.load(Ordering::SeqCst), input.len());
    control.stop(); phase(&control, Phase::Cancelled).await;
    thread.join().unwrap(); timeout(Duration::from_secs(2), server).await.unwrap().unwrap();
    assert!(control.drain().stream.packets.is_empty());
}

#[tokio::test(flavor = "current_thread")]
async fn sshPtyAndShellRefusalNeverReportConnected() {
    for (pty, shell, expected_shell) in [(false, true, 0), (true, false, 1)] {
        let (port, counts, server) = server(pty, shell).await;
        let (control, thread) = worker(port); let key = challenge(&control).await;
        assert!(control.host_answer(&key.request, true));
        timeout(Duration::from_secs(5), async {
            loop {
                let state = control.drain(); assert_ne!(state.phase, Phase::Connected);
                if state.phase == Phase::Failed { assert!(state.error.is_some()); break; }
                sleep(Duration::from_millis(2)).await;
            }
        }).await.unwrap();
        thread.join().unwrap(); timeout(Duration::from_secs(2), server).await.unwrap().unwrap();
        assert_eq!(counts.auth.load(Ordering::SeqCst), 1);
        assert_eq!(counts.shell.load(Ordering::SeqCst), expected_shell);
    }
}

#[tokio::test(flavor = "current_thread")]
async fn sshCancelDuringResolveAndTrustRejectsLateAnswers() {
    let auth = ssh::password_auth(b"synthetic-loopback-password".to_vec()).unwrap();
    let (control, backend) = transport::create(target(22), auth, || {}).unwrap();
    let thread = std::thread::spawn(move || backend.run());
    phase(&control, Phase::Resolving).await; control.stop(); phase(&control, Phase::Cancelled).await;
    assert!(!control.resolved(vec!["127.0.0.1".into()])); thread.join().unwrap();

    let (port, counts, server) = server(true, true).await;
    let (control, thread) = worker(port); let key = challenge(&control).await;
    control.stop(); assert!(!control.host_answer(&key.request, true));
    phase(&control, Phase::Cancelled).await; thread.join().unwrap();
    timeout(Duration::from_secs(2), server).await.unwrap().unwrap();
    assert_eq!(counts.auth.load(Ordering::SeqCst), 0);
}

#[tokio::test(flavor = "current_thread")]
async fn sshCancelDuringResolveConnectTrustAuthAndOutput() {
    // Owned TCP peer deliberately never sends an SSH identification string.
    let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)).await.unwrap();
    let (control, thread) = worker(listener.local_addr().unwrap().port());
    let (socket, _) = timeout(Duration::from_secs(2), listener.accept()).await.unwrap().unwrap();
    phase(&control, Phase::Handshaking).await; control.stop(); phase(&control, Phase::Cancelled).await;
    thread.join().unwrap(); drop(socket); drop(listener);

    let gate = Arc::new(tokio::sync::Notify::new());
    let (port, counts, server) = server_with(true, true, Some(gate.clone()), false).await;
    let (control, thread) = worker(port); let key = challenge(&control).await;
    assert!(control.host_answer(&key.request, true));
    timeout(Duration::from_secs(5), async { while counts.auth.load(Ordering::SeqCst) == 0 { sleep(Duration::from_millis(2)).await; } }).await.unwrap();
    control.stop(); phase(&control, Phase::Cancelled).await; thread.join().unwrap();
    gate.notify_one(); timeout(Duration::from_secs(2), server).await.unwrap().unwrap();
    assert_eq!(counts.shell.load(Ordering::SeqCst), 0);

    let (port, _, server) = server_with(true, true, None, true).await;
    let (control, thread) = worker(port); let key = challenge(&control).await;
    assert!(control.host_answer(&key.request, true)); phase(&control, Phase::Connected).await;
    timeout(Duration::from_secs(5), async {
        while control.stats().output_bytes < 1024 * 1024 - 32768 { sleep(Duration::from_millis(2)).await; }
    }).await.unwrap();
    assert!(control.stats().output_bytes <= 1024 * 1024); assert!(control.stats().output_entries <= 64);
    control.stop(); phase(&control, Phase::Cancelled).await; thread.join().unwrap();
    timeout(Duration::from_secs(2), server).await.unwrap().unwrap();
    assert!(control.drain().stream.packets.is_empty());
}

struct OwnedKey(std::path::PathBuf);
impl Drop for OwnedKey { fn drop(&mut self) { std::fs::remove_dir_all(&self.0).unwrap(); } }
#[tokio::test(flavor = "current_thread")]
async fn sshEncryptedOpenSshKeyAuthenticatesOnOwnedLoopback() {
    use russh::keys::ssh_key::{Cipher, Kdf, LineEnding};
    let root = std::path::PathBuf::from(std::env::var("CARGO_TARGET_DIR").expect("Contained Cargo target required"));
    let owned = OwnedKey(root.join("ssh-transport-keys").join(uuid::Uuid::new_v4().to_string()));
    std::fs::create_dir_all(&owned.0).unwrap(); let path = owned.0.join("key");
    let key = PrivateKey::from(Ed25519Keypair::from_seed(&[83; 32]));
    let encrypted = key.encrypt_with(Cipher::Aes256Ctr, Kdf::Bcrypt { salt: vec![19; 16], rounds: 1 }, 0x12345678, b"synthetic-key-passphrase").unwrap();
    let encoded = encrypted.to_openssh(LineEnding::LF).unwrap(); std::fs::write(&path, encoded.as_bytes()).unwrap(); drop(encoded); drop(encrypted); drop(key);
    let (port, counts, server) = server(true, true).await;
    let mut target = target(port); target.auth = "private_key".into(); target.key_path = path.to_str().unwrap().into();
    let auth = ssh::private_key_auth(target.key_path.clone(), b"synthetic-key-passphrase".to_vec()).unwrap();
    let (control, backend) = transport::create(target, auth, || {}).unwrap();
    assert!(control.resolved(vec!["127.0.0.1".into()])); let thread = std::thread::spawn(move || backend.run());
    let challenge = challenge(&control).await; assert_eq!(counts.auth.load(Ordering::SeqCst), 0);
    assert!(control.host_answer(&challenge.request, true)); phase(&control, Phase::Connected).await;
    assert_eq!(counts.auth.load(Ordering::SeqCst), 1); assert_eq!(counts.shell.load(Ordering::SeqCst), 1);
    control.stop(); phase(&control, Phase::Cancelled).await; thread.join().unwrap();
    timeout(Duration::from_secs(2), server).await.unwrap().unwrap();
}
