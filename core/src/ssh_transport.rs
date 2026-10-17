use super::{Authentication, AuthenticationKind, Failure, ffi::{BackendTarget, HostKeyIdentity}, stream::{self, StreamCommand, StreamControl, StreamDrain, StreamWorker}};
use russh::{client, Channel, ChannelMsg, ChannelReadHalf};
use russh::keys::{PublicKeyOrCertificate, PrivateKeyWithHashAlg, HashAlg};
use std::{future::Future, net::{IpAddr, SocketAddr}, sync::{Arc, Mutex, atomic::{AtomicBool, AtomicU64, Ordering}}, time::Duration};
use tokio::{net::TcpStream, sync::{oneshot, watch}, time::{timeout, sleep, Instant}};

const STAGE: Duration = Duration::from_secs(30);
const TRUST: Duration = Duration::from_secs(120);
static NEXT_REQUEST: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase { Resolving, Connecting, Handshaking, WaitingTrust, Authenticating, Opening, Connected, Disconnected, Cancelled, Failed }
#[derive(Clone)]
pub struct HostChallenge { pub request: String, pub identity: HostKeyIdentity }
pub struct BackendDrain { pub phase: Phase, pub error: Option<Failure>, pub host: Option<HostChallenge>, pub stream: StreamDrain }
struct PendingHost { request: String, answer: oneshot::Sender<bool> }
struct Metadata { error: Option<Failure>, host: Option<HostChallenge>, pending_host: Option<PendingHost>, resolved: Option<oneshot::Sender<Vec<IpAddr>>> }
struct Wake { pending: AtomicBool, callback: Box<dyn Fn() + Send + Sync> }
impl Wake {
    fn notify(&self) { if !self.pending.swap(true, Ordering::AcqRel) { (self.callback)(); } }
}
#[derive(Clone)]
pub struct BackendControl { stream: StreamControl, metadata: Arc<Mutex<Metadata>>, phase: watch::Sender<Phase>, wake: Arc<Wake> }
pub struct Backend { target: BackendTarget, auth: Option<Box<Authentication>>, control: BackendControl, output: StreamWorker, resolved: Option<oneshot::Receiver<Vec<IpAddr>>> }

pub fn create(target: BackendTarget, auth: Box<Authentication>, wake: impl Fn() + Send + Sync + 'static) -> Result<(BackendControl, Backend), Failure> {
    crate::model::validate_ssh_target(&target.host, target.port, &target.username, &target.key_path).map_err(|_| Failure::Target)?;
    if target.username.is_empty() { return Err(Failure::Target); }
    if !(1..=256).contains(&target.rows) || !(1..=512).contains(&target.columns) { return Err(Failure::TerminalSize); }
    if target.command.len() > 4096 || target.command.contains('\0') { return Err(Failure::Target); }
    match (&auth.kind, target.auth.as_str()) {
        (AuthenticationKind::Password(_), "password") => {},
        (AuthenticationKind::PrivateKey { path, .. }, "private_key") if path == &target.key_path => {},
        _ => return Err(Failure::AuthenticationMethod),
    }
    let wake = Arc::new(Wake { pending: AtomicBool::new(false), callback: Box::new(wake) });
    let stream_wake = wake.clone(); let (stream, output) = stream::byte_stream(move || stream_wake.notify());
    let (phase, _) = watch::channel(Phase::Resolving); let (send, resolved) = oneshot::channel();
    let control = BackendControl { stream, metadata: Arc::new(Mutex::new(Metadata { error: None, host: None, pending_host: None, resolved: Some(send) })), phase, wake };
    let backend = Backend { target, auth: Some(auth), control: control.clone(), output, resolved: Some(resolved) };
    Ok((control, backend))
}
impl BackendControl {
    pub fn poll(&self) -> super::ffi::BackendEvent {
        let drained = self.drain();
        let phase = match drained.phase {
            Phase::Resolving => 0, Phase::Connecting => 1, Phase::Handshaking => 2, Phase::WaitingTrust => 3,
            Phase::Authenticating => 4, Phase::Opening => 5, Phase::Connected => 6,
            Phase::Disconnected => 7, Phase::Cancelled => 8, Phase::Failed => 9,
        };
        let host_pending = drained.host.is_some();
        let (host_request, host_key) = match drained.host {
            Some(host) => (host.request, host.identity),
            None => (String::new(), HostKeyIdentity { host: String::new(), port: 0, algorithm: String::new(), key: Vec::new(), fingerprint: String::new() }),
        };
        super::ffi::BackendEvent {
            phase, error: drained.error.map_or_else(String::new, |error| error.to_string()), host_pending, host_request, host_key,
            packets: drained.stream.packets.into_iter().map(|bytes| super::ffi::OutputPacket { bytes }).collect(), ended: drained.stream.ended,
        }
    }
    pub fn input(&self, bytes: &[u8]) -> Result<(), Failure> {
        if *self.phase.borrow() != Phase::Connected { return Err(Failure::StreamClosed); }
        self.stream.input(bytes)
    }
    pub fn resize(&self, rows: u16, columns: u16) -> Result<(), Failure> { self.stream.resize(rows, columns) }
    pub fn stop(&self) { self.stream.stop(); }
    pub fn stats(&self) -> stream::StreamStats { self.stream.stats() }
    pub fn resolved(&self, addresses: Vec<String>) -> bool {
        if self.stream.is_stopped() || addresses.len() > 16 { return false; }
        let parsed: Vec<IpAddr> = addresses.iter().filter_map(|address| address.parse().ok()).collect();
        let mut metadata = self.metadata.lock().unwrap();
        if self.stream.is_stopped() { return false; }
        let Some(send) = metadata.resolved.take() else { return false; };
        send.send(parsed).is_ok()
    }
    pub fn host_answer(&self, request: &str, allowed: bool) -> bool {
        if self.stream.is_stopped() { return false; }
        let mut metadata = self.metadata.lock().unwrap();
        if self.stream.is_stopped() || !metadata.pending_host.as_ref().is_some_and(|pending| pending.request == request) { return false; }
        let pending = metadata.pending_host.take().unwrap(); metadata.host = None;
        pending.answer.send(allowed).is_ok()
    }
    pub fn drain(&self) -> BackendDrain {
        // Clear before reading so a concurrent producer can queue the next wake.
        self.wake.pending.store(false, Ordering::Release);
        let (error, host) = { let mut metadata = self.metadata.lock().unwrap(); (metadata.error, metadata.host.take()) };
        BackendDrain { phase: *self.phase.borrow(), error, host, stream: self.stream.drain() }
    }
    fn phase(&self, phase: Phase) { self.phase.send_replace(phase); self.wake.notify(); }
    fn complete(&self, result: Result<(), Failure>) {
        let mut metadata = self.metadata.lock().unwrap(); metadata.pending_host = None; metadata.host = None; metadata.resolved = None;
        let phase = match result {
            _ if self.stream.is_stopped() => Phase::Cancelled,
            Ok(()) => Phase::Disconnected,
            Err(Failure::Cancelled) => Phase::Cancelled,
            Err(error) => { metadata.error = Some(error); Phase::Failed },
        };
        drop(metadata); self.phase(phase);
    }
    async fn guard<T>(&self, limit: Duration, future: impl Future<Output = Result<T, Failure>>) -> Result<T, Failure> {
        tokio::select! { biased;
            _ = self.stream.cancelled() => Err(Failure::Cancelled),
            result = timeout(limit, future) => result.map_err(|_| Failure::Deadline)?,
        }
    }
}
impl Backend {
    pub fn control(&self) -> Box<BackendControl> { Box::new(self.control.clone()) }
    // Called once on the owning native worker. Dropping the current-thread runtime
    // cancels even russh tasks detached by an interrupted connect_stream handshake.
    // The final phase is published only after runtime/socket cleanup, not on EOF.
    pub fn run(mut self) {
        let runtime = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
            Ok(runtime) => runtime,
            Err(_) => { self.control.complete(Err(Failure::Runtime)); return; },
        };
        self.control.wake.notify();
        let result = runtime.block_on(self.connect());
        self.auth = None; drop(runtime);
        self.control.complete(result); self.output.finish();
    }
    async fn connect(&mut self) -> Result<(), Failure> {
        let addresses = self.control.guard(STAGE, async {
            self.resolved.take().ok_or(Failure::Resolve)?.await.map_err(|_| Failure::Resolve)
        }).await?;
        if addresses.is_empty() { return Err(Failure::Resolve); }
        self.control.phase(Phase::Connecting);
        let socket = self.control.guard(STAGE, async {
            for address in addresses {
                if let Ok(Ok(socket)) = timeout(Duration::from_secs(5), TcpStream::connect(SocketAddr::new(address, self.target.port))).await { return Ok(socket); }
            }
            Err(Failure::Connect)
        }).await?;
        socket.set_nodelay(true).map_err(|_| Failure::Connect)?;
        let config = Arc::new(client::Config { window_size: 256 * 1024, maximum_packet_size: stream::PACKET_BYTES as u32, channel_buffer_size: 8, ..Default::default() });
        self.control.phase(Phase::Handshaking);
        let handler = Handler { control: self.control.clone(), host: self.target.host.clone(), port: self.target.port };
        let mut connection = handshake(&self.control, client::connect_stream(config, socket, handler)).await?;
        self.control.phase(Phase::Authenticating);
        let authentication = self.auth.take().ok_or(Failure::AuthenticationMethod)?;
        let result = match authentication.kind {
            AuthenticationKind::Password(password) => {
                let text = std::str::from_utf8(password.expose()).map_err(|_| Failure::AuthenticationBounds)?;
                let result = self.control.guard(STAGE, async {
                    connection.authenticate_password(self.target.username.clone(), text).await.map_err(|_| Failure::AuthenticationRejected)
                }).await;
                drop(password); result?
            },
            AuthenticationKind::PrivateKey { path, passphrase } => {
                let bytes = super::read_private_key(&path, || self.control.stream.is_stopped())?;
                let key = super::decode_private_key(bytes, &passphrase)?; drop(passphrase);
                if self.control.stream.is_stopped() { return Err(Failure::Cancelled); }
                // Never fall back to RSA/SHA-1 when the server lacks SHA-2 support.
                let hash = if key.algorithm().is_rsa() {
                    let hash = self.control.guard(STAGE, async { connection.best_supported_rsa_hash().await.map_err(|_| Failure::KeyAlgorithm) }).await?.flatten();
                    match hash { Some(HashAlg::Sha256 | HashAlg::Sha512) => hash, _ => return Err(Failure::KeyAlgorithm) }
                } else { None };
                self.control.guard(STAGE, async {
                    connection.authenticate_publickey(self.target.username.clone(), PrivateKeyWithHashAlg::new(Arc::new(key), hash)).await.map_err(|_| Failure::AuthenticationRejected)
                }).await?
            },
        };
        if !result.success() { return Err(Failure::AuthenticationRejected); }
        self.control.phase(Phase::Opening);
        let channel = self.control.guard(STAGE, async { connection.channel_open_session().await.map_err(|_| Failure::Channel) }).await?;
        let (mut reader, writer) = channel.split();
        if self.target.command.is_empty() {
            self.control.guard(STAGE, async {
                writer.request_pty(true, "xterm-256color", self.target.columns.into(), self.target.rows.into(), 0, 0, &[]).await.map_err(|_| Failure::Pty)?;
                reply(&mut reader, &mut self.output, Failure::Pty).await
            }).await?;
            self.control.guard(STAGE, async {
                writer.request_shell(true).await.map_err(|_| Failure::Shell)?;
                reply(&mut reader, &mut self.output, Failure::Shell).await
            }).await?;
        } else {
            let command = self.target.command.clone();
            self.control.guard(STAGE, async {
                writer.exec(true, command.into_bytes()).await.map_err(|_| Failure::Shell)?;
                loop {
                    match reader.wait().await {
                        Some(ChannelMsg::Success) => return Ok(()),
                        Some(ChannelMsg::Data { data }) => { if !data.is_empty() { self.output.output(data.to_vec()).await?; } },
                        Some(ChannelMsg::ExtendedData { ext: 1, .. } | ChannelMsg::WindowAdjusted { .. }) => {},
                        _ => return Err(Failure::Shell),
                    }
                }
            }).await?;
        }
        self.control.phase(Phase::Connected);
        let mut commands = self.output.take_commands().ok_or(Failure::Channel)?;
        let control = self.control.clone();
        // A command's stdout is its protocol (SCP); its stderr (shell startup noise) is not.
        let exec = !self.target.command.is_empty();
        let read = async {
            loop {
                match reader.wait().await {
                    Some(ChannelMsg::ExtendedData { ext: 1, .. }) if exec => {},
                    Some(ChannelMsg::Data { data } | ChannelMsg::ExtendedData { data, ext: 1 }) => {
                        if data.len() > stream::PACKET_BYTES { return Err(Failure::StreamBounds); }
                        if !data.is_empty() { self.output.output(data.to_vec()).await?; }
                    },
                    None | Some(ChannelMsg::Eof | ChannelMsg::Close | ChannelMsg::ExitStatus { .. } | ChannelMsg::ExitSignal { .. }) => return Ok(()),
                    Some(ChannelMsg::Failure | ChannelMsg::OpenFailure(_) | ChannelMsg::ExtendedData { .. }) => return Err(Failure::Channel),
                    _ => {},
                }
            }
        };
        let write = async {
            loop {
                match commands.next_command().await? {
                    StreamCommand::Input(bytes) => control.guard(STAGE, async { writer.data_bytes(bytes).await.map_err(|_| Failure::Channel) }).await?,
                    StreamCommand::Resize(size) => control.guard(STAGE, async { writer.window_change(size.columns.into(), size.rows.into(), 0, 0).await.map_err(|_| Failure::Channel) }).await?,
                }
            }
        };
        tokio::select! { biased;
            _ = self.control.stream.cancelled() => Err(Failure::Cancelled),
            result = read => result,
            result = write => result,
        }
    }
}
async fn reply(reader: &mut ChannelReadHalf, output: &mut StreamWorker, failure: Failure) -> Result<(), Failure> {
    loop {
        match reader.wait().await {
            Some(ChannelMsg::Success) => return Ok(()),
            Some(ChannelMsg::Data { data } | ChannelMsg::ExtendedData { data, ext: 1 }) => {
                if !data.is_empty() { output.output(data.to_vec()).await?; }
            },
            Some(ChannelMsg::WindowAdjusted { .. }) => {},
            _ => return Err(failure),
        }
    }
}
async fn handshake(control: &BackendControl, future: impl Future<Output = Result<client::Handle<Handler>, Failure>>) -> Result<client::Handle<Handler>, Failure> {
    let mut phase = control.phase.subscribe(); let deadline = sleep(STAGE); tokio::pin!(deadline); tokio::pin!(future);
    loop {
        tokio::select! { biased;
            _ = control.stream.cancelled() => return Err(Failure::Cancelled),
            result = &mut future => return result,
            _ = phase.changed() => {
                let limit = if *phase.borrow_and_update() == Phase::WaitingTrust { TRUST } else { STAGE };
                deadline.as_mut().reset(Instant::now() + limit);
            },
            _ = &mut deadline => return Err(Failure::Deadline),
        }
    }
}
impl From<russh::Error> for Failure { fn from(_: russh::Error) -> Self { Failure::Handshake } }
struct Handler { control: BackendControl, host: String, port: u16 }
impl client::Handler for Handler {
    type Error = Failure;
    async fn check_server_key(&mut self, key: &PublicKeyOrCertificate) -> Result<bool, Failure> {
        let PublicKeyOrCertificate::PublicKey { key, .. } = key else { return Err(Failure::HostKey); };
        let identity = super::host_key_identity(self.host.clone(), self.port, key.to_bytes().map_err(|_| Failure::HostKey)?)?;
        let (send, receive) = oneshot::channel();
        {
            let mut metadata = self.control.metadata.lock().unwrap();
            if self.control.stream.is_stopped() { return Err(Failure::Cancelled); }
            let request = NEXT_REQUEST.fetch_add(1, Ordering::Relaxed).to_string();
            metadata.host = Some(HostChallenge { request: request.clone(), identity });
            metadata.pending_host = Some(PendingHost { request, answer: send });
        }
        self.control.phase(Phase::WaitingTrust);
        let allowed = self.control.guard(TRUST, async { receive.await.map_err(|_| Failure::HostRejected) }).await?;
        if !allowed { return Err(Failure::HostRejected); }
        self.control.phase(Phase::Handshaking); Ok(true)
    }
    // russh accepts these by default. Dropping each reply explicitly rejects it.
    async fn server_channel_open_forwarded_tcpip(&mut self, _: Channel<client::Msg>, _: &str, _: u32, _: &str, _: u32, reply: client::ChannelOpenHandle, _: &mut client::Session) -> Result<(), Failure> { drop(reply); Ok(()) }
    async fn server_channel_open_forwarded_streamlocal(&mut self, _: Channel<client::Msg>, _: &str, reply: client::ChannelOpenHandle, _: &mut client::Session) -> Result<(), Failure> { drop(reply); Ok(()) }
    async fn server_channel_open_agent_forward(&mut self, _: Channel<client::Msg>, reply: client::ChannelOpenHandle, _: &mut client::Session) -> Result<(), Failure> { drop(reply); Ok(()) }
    async fn server_channel_open_session(&mut self, _: Channel<client::Msg>, reply: client::ChannelOpenHandle, _: &mut client::Session) -> Result<(), Failure> { drop(reply); Ok(()) }
    async fn server_channel_open_direct_tcpip(&mut self, _: Channel<client::Msg>, _: &str, _: u32, _: &str, _: u32, reply: client::ChannelOpenHandle, _: &mut client::Session) -> Result<(), Failure> { drop(reply); Ok(()) }
    async fn server_channel_open_direct_streamlocal(&mut self, _: Channel<client::Msg>, _: &str, reply: client::ChannelOpenHandle, _: &mut client::Session) -> Result<(), Failure> { drop(reply); Ok(()) }
    async fn server_channel_open_x11(&mut self, _: Channel<client::Msg>, _: &str, _: u32, reply: client::ChannelOpenHandle, _: &mut client::Session) -> Result<(), Failure> { drop(reply); Ok(()) }
}
