use super::{Authentication, AuthenticationKind, Failure, valid_password, ffi::{BackendTarget, BackendEvent, GraphicsEvent}};
use crate::credentials::Secret;
use std::{future::Future, io, net::{IpAddr, SocketAddr}, pin::Pin, sync::{Arc, Mutex, atomic::{AtomicBool, AtomicU8, AtomicU32, Ordering}}, task::{Context, Poll}, time::Duration};
use tokio::{io::{AsyncRead, AsyncWrite, ReadBuf}, net::TcpStream, sync::{mpsc, oneshot, watch}, time::{Instant, Sleep, sleep, sleep_until, timeout}};
use vnc::{VncClient, VncConnector, VncEncoding, VncEvent, X11Event, PixelFormat};
use vnc::client::connector::AuthenticationPolicy;

pub const INPUT_CAPACITY: usize = 128;
const EVENTS_PER_TURN: usize = 16;
const POLL_INTERVAL: Duration = Duration::from_millis(4);
const REFRESH_INTERVAL: Duration = Duration::from_millis(16);
const MAX_SIDE: u16 = 8192;
const MAX_PIXELS: usize = 8_294_400;
const MAX_CURSOR_SIDE: u16 = 256;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// PasswordRequired: the server selected VncAuth with no saved password; the
// handshake waits (without its stage deadline) for BackendControl::password.
pub enum Phase { Resolving, Connecting, Handshaking, Connected, Disconnected, Cancelled, Failed, PasswordRequired }
#[derive(Clone, Copy)]
pub struct Timeouts { pub stage: Duration, pub address: Duration, pub write: Duration }
impl Default for Timeouts {
    fn default() -> Self { Self { stage: Duration::from_secs(30), address: Duration::from_secs(5), write: Duration::from_secs(30) } }
}
struct Metadata {
    phase: Phase, error: Option<Failure>, resolved: Option<oneshot::Sender<Vec<IpAddr>>>,
    password: Option<oneshot::Sender<Secret>>, prompting: bool, resumed: Option<Instant>,
}
struct Wake { pending: AtomicBool, callback: Box<dyn Fn() + Send + Sync> }
impl Wake {
    fn notify(&self) { if !self.pending.swap(true, Ordering::AcqRel) { (self.callback)(); } }
}
struct Shared {
    metadata: Mutex<Metadata>,
    // 0 running, 1 caller cancellation, 2 transition overload. Separate from
    // input/network/event queues so full queues can never prevent shutdown.
    reason: AtomicU8,
    cancel: watch::Sender<bool>,
    geometry: AtomicU32,
    view_only: bool,
    wake: Wake,
}
#[derive(Clone)]
pub struct BackendControl { shared: Arc<Shared>, input: mpsc::Sender<X11Event> }
pub struct Backend {
    target: BackendTarget,
    auth: Option<Box<Authentication>>,
    control: BackendControl,
    input: mpsc::Receiver<X11Event>,
    resolved: Option<oneshot::Receiver<Vec<IpAddr>>>,
    graphics: Box<dyn Fn(&GraphicsEvent) -> bool + Send>,
    timeouts: Timeouts,
}

pub fn create(target: BackendTarget, auth: Box<Authentication>, graphics: impl Fn(&GraphicsEvent) -> bool + Send + 'static, wake: impl Fn() + Send + Sync + 'static) -> Result<(BackendControl, Backend), Failure> {
    create_with_timeouts(target, auth, graphics, wake, Timeouts::default())
}
pub fn create_with_timeouts(target: BackendTarget, auth: Box<Authentication>, graphics: impl Fn(&GraphicsEvent) -> bool + Send + 'static, wake: impl Fn() + Send + Sync + 'static, timeouts: Timeouts) -> Result<(BackendControl, Backend), Failure> {
    // Qt supplies async DNS results; this validates only, never resolves on GUI.
    if target.host.is_empty() || target.host.len() > 253 || target.host.trim() != target.host || target.port == 0
        || target.host.chars().any(|c| c.is_control() || c.is_whitespace() || matches!(c, '/' | '\\' | '[' | ']')) {
        return Err(Failure::Target);
    }
    let (cancel, _) = watch::channel(false);
    let (send, resolved) = oneshot::channel();
    let (input, receiver) = mpsc::channel(INPUT_CAPACITY);
    let shared = Arc::new(Shared {
        metadata: Mutex::new(Metadata { phase: Phase::Resolving, error: None, resolved: Some(send), password: None, prompting: false, resumed: None }),
        reason: AtomicU8::new(0), cancel, geometry: AtomicU32::new(0), view_only: target.view_only,
        wake: Wake { pending: AtomicBool::new(false), callback: Box::new(wake) },
    });
    let control = BackendControl { shared, input };
    let backend = Backend { target, auth: Some(auth), control: control.clone(), input: receiver, resolved: Some(resolved), graphics: Box::new(graphics), timeouts };
    Ok((control, backend))
}
impl BackendControl {
    pub fn poll(&self) -> BackendEvent {
        // Clear before reading, so changes racing with this read queue a new wake.
        self.shared.wake.pending.store(false, Ordering::Release);
        let state = self.shared.metadata.lock().unwrap();
        BackendEvent { phase: state.phase as u8, error: state.error.map_or_else(String::new, |error| error.to_string()) }
    }
    // Answers a PasswordRequired request. Invalid passwords are rejected without
    // consuming the request, so the GUI can ask again.
    pub fn password(&self, password: Vec<u8>) -> Result<(), Failure> {
        let password = valid_password(password)?;
        if self.shared.reason.load(Ordering::Acquire) != 0 { return Err(Failure::Closed); }
        let send = self.shared.metadata.lock().unwrap().password.take().ok_or(Failure::Closed)?;
        send.send(password).map_err(|_| Failure::Closed)
    }
    async fn request_password(&self) -> Option<Secret> {
        let (send, receive) = oneshot::channel();
        {
            let mut state = self.shared.metadata.lock().unwrap();
            state.password = Some(send); state.prompting = true; state.phase = Phase::PasswordRequired;
        }
        self.shared.wake.notify();
        let password = receive.await.ok();
        {
            let mut state = self.shared.metadata.lock().unwrap();
            state.password = None; state.prompting = false; state.resumed = Some(Instant::now());
            if state.phase == Phase::PasswordRequired { state.phase = Phase::Handshaking; }
        }
        self.shared.wake.notify();
        password
    }
    pub fn key(&self, keysym: u32, down: bool) -> Result<(), Failure> {
        if keysym == 0 { return Err(Failure::InputBounds); }
        self.send(X11Event::KeyEvent((keysym, down).into()))
    }
    pub fn pointer(&self, mask: u8, x: u16, y: u16) -> Result<(), Failure> {
        let size = self.shared.geometry.load(Ordering::Acquire);
        if x >= (size >> 16) as u16 || y >= size as u16 { return Err(Failure::InputBounds); }
        self.send(X11Event::PointerEvent((x, y, mask).into()))
    }
    fn send(&self, event: X11Event) -> Result<(), Failure> {
        if self.shared.view_only { return Err(Failure::ViewOnly); }
        if self.shared.reason.load(Ordering::Acquire) != 0 || self.shared.metadata.lock().unwrap().phase != Phase::Connected { return Err(Failure::Closed); }
        match self.input.try_send(event) {
            Ok(()) => Ok(()),
            Err(mpsc::error::TrySendError::Closed(_)) => Err(Failure::Closed),
            Err(mpsc::error::TrySendError::Full(_)) => {
                // Never silently lose a key/button release. No motion coalescing is
                // needed; fail visibly rather than guessing which input is replaceable.
                if self.shared.reason.compare_exchange(0, 2, Ordering::AcqRel, Ordering::Acquire).is_ok() {
                    self.shared.cancel.send_replace(true);
                }
                Err(Failure::InputOverload)
            },
        }
    }
    pub fn stop(&self) {
        let _ = self.shared.reason.compare_exchange(0, 1, Ordering::AcqRel, Ordering::Acquire);
        self.shared.cancel.send_replace(true);
    }
    pub fn resolved(&self, addresses: Vec<String>) -> bool {
        if self.shared.reason.load(Ordering::Acquire) != 0 || addresses.len() > 16 { return false; }
        let mut parsed = Vec::new();
        for address in addresses {
            if address.len() > 64 { return false; }
            let Ok(address) = address.parse::<IpAddr>() else { return false; };
            if !parsed.contains(&address) { parsed.push(address); }
        }
        let mut state = self.shared.metadata.lock().unwrap();
        if self.shared.reason.load(Ordering::Acquire) != 0 { return false; }
        let Some(send) = state.resolved.take() else { return false; };
        send.send(parsed).is_ok()
    }
    fn phase(&self, phase: Phase) {
        self.shared.metadata.lock().unwrap().phase = phase;
        self.shared.wake.notify();
    }
    fn complete(&self, result: Result<(), Failure>) {
        let mut state = self.shared.metadata.lock().unwrap();
        state.resolved = None; state.password = None;
        let result = match self.shared.reason.load(Ordering::Acquire) {
            1 => Err(Failure::Cancelled), 2 => Err(Failure::InputOverload), _ => result,
        };
        state.phase = match result {
            Ok(()) => Phase::Disconnected,
            Err(Failure::Cancelled) => Phase::Cancelled,
            Err(error) => { state.error = Some(error); Phase::Failed },
        };
        drop(state);
        self.shared.wake.notify();
    }
    async fn cancelled(&self) {
        let mut cancel = self.shared.cancel.subscribe();
        if *cancel.borrow_and_update() { return; }
        let _ = cancel.changed().await;
    }
    // Like guard, but time spent waiting for the user's password does not count.
    async fn handshake_guard<T>(&self, limit: Duration, future: impl Future<Output = Result<T, Failure>>) -> Result<T, Failure> {
        tokio::pin!(future);
        let mut deadline = Instant::now() + limit;
        loop {
            tokio::select! { biased;
                _ = self.cancelled() => return Err(Failure::Cancelled),
                result = &mut future => return result,
                _ = sleep_until(deadline) => {
                    let state = self.shared.metadata.lock().unwrap(); let now = Instant::now();
                    if state.prompting { deadline = now + limit; }
                    else if let Some(resumed) = state.resumed.filter(|resumed| *resumed + limit > now) { deadline = resumed + limit; }
                    else { return Err(Failure::Deadline); }
                },
            }
        }
    }
    async fn guard<T>(&self, limit: Duration, future: impl Future<Output = Result<T, Failure>>) -> Result<T, Failure> {
        tokio::select! { biased;
            _ = self.cancelled() => Err(Failure::Cancelled),
            result = timeout(limit, future) => result.map_err(|_| Failure::Deadline)?,
        }
    }
}
impl Backend {
    pub fn control(&self) -> Box<BackendControl> { Box::new(self.control.clone()) }
    pub fn run(mut self) {
        let runtime = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
            Ok(runtime) => runtime,
            Err(_) => { self.auth = None; self.input.close(); self.control.complete(Err(Failure::Runtime)); return; },
        };
        self.control.shared.wake.notify();
        let result = runtime.block_on(self.connect());
        self.auth = None;
        self.input.close();
        // All library decoder/network tasks are confined to this runtime. Drop it
        // before publishing terminal metadata so native retirement observes cleanup.
        drop(runtime);
        self.control.complete(result);
    }
    async fn connect(&mut self) -> Result<(), Failure> {
        let control = self.control.clone();
        let addresses = control.guard(self.timeouts.stage, async {
            self.resolved.take().ok_or(Failure::Resolve)?.await.map_err(|_| Failure::Resolve)
        }).await?;
        if addresses.is_empty() { return Err(Failure::Resolve); }
        control.phase(Phase::Connecting);
        let socket = control.guard(self.timeouts.stage, async {
            for address in addresses {
                if let Ok(Ok(socket)) = timeout(self.timeouts.address, TcpStream::connect(SocketAddr::new(address, self.target.port))).await { return Ok(socket); }
            }
            Err(Failure::Connect)
        }).await?;
        socket.set_nodelay(true).map_err(|_| Failure::Connect)?;
        control.phase(Phase::Handshaking);
        let authentication = self.auth.take().ok_or(Failure::Handshake)?;
        let password = match authentication.kind {
            AuthenticationKind::Password(password) => Some(password),
            AuthenticationKind::Prompt => None,
        };
        let request = control.clone();
        let write_expired = Arc::new(AtomicBool::new(false));
        let socket = DeadlineStream { socket, limit: self.timeouts.write, deadline: None, expired: write_expired.clone() };
        let client = control.handshake_guard(self.timeouts.stage, async {
            VncConnector::new(socket)
                // None whenever offered (nothing is sent); otherwise VncAuth.
                .set_authentication_policy(AuthenticationPolicy::NoneIfOffered)
                .set_auth_method(async move {
                    // Only polled once the server selected VncAuth. Upstream requires one
                    // String copy; its vendored credential guard wipes it on drop.
                    let password = match password { Some(password) => password, None => request.request_password().await.ok_or(vnc::VncError::NoPassword)? };
                    let text = std::str::from_utf8(password.expose()).map_err(|_| vnc::VncError::NoPassword)?.to_owned();
                    drop(password); Ok(text)
                })
                .set_pixel_format(PixelFormat::bgra())
                .allow_shared(true)
                // Preference order: ZRLE, then Tight and TRLE for servers without
                // ZRLE (all compressed, far less bandwidth than Raw), CopyRect, Raw.
                .add_encoding(VncEncoding::Zrle)
                .add_encoding(VncEncoding::Tight)
                .add_encoding(VncEncoding::Trle)
                .add_encoding(VncEncoding::CopyRect)
                .add_encoding(VncEncoding::Raw)
                .add_encoding(VncEncoding::LastRectPseudo)
                .add_encoding(VncEncoding::CursorPseudo)
                .add_encoding(VncEncoding::DesktopSizePseudo)
                .build().map_err(|_| Failure::Handshake)?
                .try_start().await.map_err(|error| if write_expired.load(Ordering::Acquire) { Failure::Deadline }
                    else if matches!(error, vnc::VncError::WrongPassword) { Failure::Rejected }
                    else { Failure::Handshake })?
                .finish().map_err(|_| Failure::Handshake)
        }).await?;
        let result = self.connected(&client, &write_expired).await;
        // close never holds a mutex across recv_event: this backend only polls.
        let _ = client.close().await;
        drop(client);
        result
    }
    async fn connected(&mut self, client: &VncClient, write_expired: &AtomicBool) -> Result<(), Failure> {
        let control = self.control.clone();
        let mut geometry = (0, 0);
        // The library already enqueued exactly one initial non-incremental request.
        let mut outstanding = true;
        let mut refresh_at: Option<Instant> = None;
        let mut announced = false;
        let (mut dirty, mut pixels) = (false, false);
        loop {
            if control.shared.reason.load(Ordering::Acquire) != 0 { return Err(Failure::Cancelled); }
            if write_expired.load(Ordering::Acquire) { return Err(Failure::Deadline); }
            let mut productive = false;
            for _ in 0..EVENTS_PER_TURN {
                let event = match client.poll_event().await {
                    Ok(Some(event)) => event,
                    Ok(None) => break,
                    Err(vnc::VncError::ClientNotRunning) if !write_expired.load(Ordering::Acquire) => return Ok(()),
                    Err(_) => return Err(if write_expired.load(Ordering::Acquire) { Failure::Deadline } else { Failure::Protocol }),
                };
                productive = true;
                match event {
                    VncEvent::UpdateComplete => {
                        if !outstanding { return Err(Failure::Protocol); }
                        outstanding = false;
                        // Publish one complete FramebufferUpdate: the GUI must never
                        // present a half-applied update (torn window drags/scrolls).
                        if !(self.graphics)(&commit_event()) { return Err(Failure::Bounds); }
                        if dirty { control.shared.wake.notify(); }
                        // Servers hold incremental requests until something changes, so
                        // re-request at once after pixel changes; pace other replies.
                        refresh_at = Some(if pixels { Instant::now() } else { Instant::now() + REFRESH_INTERVAL });
                        dirty = false; pixels = false;
                    },
                    VncEvent::Bell | VncEvent::Text(_) => {},
                    VncEvent::Error(_) => return Err(if write_expired.load(Ordering::Acquire) { Failure::Deadline } else { Failure::Protocol }),
                    event => {
                        let event = graphics_event(event, &mut geometry)?;
                        if !(self.graphics)(&event) { return Err(Failure::Bounds); }
                        if event.kind == 0 {
                            control.shared.geometry.store((u32::from(event.width) << 16) | u32::from(event.height), Ordering::Release);
                            if !announced { announced = true; control.phase(Phase::Connected); }
                        }
                        // Cursor shapes are independent of the framebuffer and wake now;
                        // pixel changes wake once at UpdateComplete after the commit.
                        if event.kind == 3 { control.shared.wake.notify(); } else { dirty = true; pixels |= event.kind != 0; }
                    },
                }
            }
            // Bounded work and explicit yield keep network/decoder tasks, input and
            // cancellation responsive, including an always-ready stream of events.
            tokio::task::yield_now().await;
            let refresh = refresh_at.unwrap_or_else(|| Instant::now() + Duration::from_secs(86400));
            tokio::select! { biased;
                _ = control.cancelled() => return Err(Failure::Cancelled),
                _ = sleep_until(refresh), if refresh_at.is_some() => {
                    if outstanding { return Err(Failure::Protocol); }
                    control.guard(self.timeouts.write, async { client.input(X11Event::Refresh).await.map_err(|_| Failure::Protocol) }).await?;
                    outstanding = true;
                    refresh_at = None;
                },
                command = self.input.recv() => {
                    let Some(command) = command else { return Err(Failure::Closed); };
                    control.guard(self.timeouts.write, async { client.input(command).await.map_err(|_| Failure::Protocol) }).await?;
                },
                // The two-event decoder queue refills during the yield above.
                // Recheck it promptly after productive turns, but first service
                // cancellation, due refresh and input. Only idle turns need polling.
                _ = async { if !productive { sleep(POLL_INTERVAL).await; } } => {},
            }
        }
    }
}

fn dimensions(width: u16, height: u16) -> Result<usize, Failure> {
    let pixels = usize::from(width) * usize::from(height);
    if width == 0 || height == 0 || width > MAX_SIDE || height > MAX_SIDE || pixels > MAX_PIXELS { return Err(Failure::Bounds); }
    Ok(pixels)
}
fn rectangle(rect: &vnc::Rect, screen: (u16, u16)) -> Result<usize, Failure> {
    let pixels = dimensions(rect.width, rect.height)?;
    if u32::from(rect.x) + u32::from(rect.width) > u32::from(screen.0)
        || u32::from(rect.y) + u32::from(rect.height) > u32::from(screen.1) { return Err(Failure::Bounds); }
    Ok(pixels)
}
fn commit_event() -> GraphicsEvent {
    GraphicsEvent { kind: 4, x: 0, y: 0, width: 0, height: 0, src_x: 0, src_y: 0, pixels: Vec::new() }
}
fn graphics_event(event: VncEvent, geometry: &mut (u16, u16)) -> Result<GraphicsEvent, Failure> {
    let empty = || GraphicsEvent { kind: 0, x: 0, y: 0, width: 0, height: 0, src_x: 0, src_y: 0, pixels: Vec::new() };
    let mut result = empty();
    match event {
        VncEvent::SetResolution(screen) => {
            dimensions(screen.width, screen.height)?;
            *geometry = (screen.width, screen.height);
            result.width = screen.width; result.height = screen.height;
        },
        VncEvent::RawImage(rect, pixels) => {
            if rectangle(&rect, *geometry)? * 4 != pixels.len() { return Err(Failure::Bounds); }
            result.kind = 1; result.x = rect.x; result.y = rect.y; result.width = rect.width; result.height = rect.height; result.pixels = pixels;
        },
        VncEvent::Copy(dst, src) => {
            rectangle(&dst, *geometry)?; rectangle(&src, *geometry)?;
            if (dst.width, dst.height) != (src.width, src.height) { return Err(Failure::Bounds); }
            result.kind = 2; result.x = dst.x; result.y = dst.y; result.width = dst.width; result.height = dst.height; result.src_x = src.x; result.src_y = src.y;
        },
        VncEvent::SetCursor(rect, pixels) => {
            result.kind = 3;
            if rect.width > MAX_CURSOR_SIDE || rect.height > MAX_CURSOR_SIDE { return Err(Failure::Bounds); }
            if rect.width == 0 || rect.height == 0 {
                if !pixels.is_empty() { return Err(Failure::Bounds); }
            } else {
                if rect.width > MAX_CURSOR_SIDE || rect.height > MAX_CURSOR_SIDE || rect.x >= rect.width || rect.y >= rect.height
                    || usize::from(rect.width) * usize::from(rect.height) * 4 != pixels.len() { return Err(Failure::Bounds); }
                result.x = rect.x; result.y = rect.y; result.width = rect.width; result.height = rect.height; result.pixels = pixels;
            }
        },
        _ => return Err(Failure::Protocol),
    }
    Ok(result)
}

// Bound socket write stalls without timing out a legitimate idle incremental
// request. The timer covers partial writes until the caller's buffer is complete;
// no read deadline is imposed after the connection-stage guard ends.
struct DeadlineStream<S> {
    socket: S,
    limit: Duration,
    deadline: Option<Pin<Box<Sleep>>>,
    expired: Arc<AtomicBool>,
}
impl<S: AsyncRead + Unpin> AsyncRead for DeadlineStream<S> {
    fn poll_read(mut self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &mut ReadBuf<'_>) -> Poll<io::Result<()>> { Pin::new(&mut self.socket).poll_read(cx, buf) }
}
impl<S> DeadlineStream<S> {
    fn poll_deadline(&mut self, cx: &mut Context<'_>) -> bool {
        let limit = self.limit;
        let timer = self.deadline.get_or_insert_with(|| Box::pin(sleep(limit)));
        if timer.as_mut().poll(cx).is_ready() { self.expired.store(true, Ordering::Release); true } else { false }
    }
    fn timed_out() -> io::Error { io::Error::new(io::ErrorKind::TimedOut, "VNC write deadline") }
}
impl<S: AsyncWrite + Unpin> AsyncWrite for DeadlineStream<S> {
    fn poll_write(mut self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &[u8]) -> Poll<io::Result<usize>> {
        if self.poll_deadline(cx) { return Poll::Ready(Err(Self::timed_out())); }
        let result = Pin::new(&mut self.socket).poll_write(cx, buf);
        if matches!(result, Poll::Ready(Ok(n)) if n == buf.len()) { self.deadline = None; }
        result
    }
    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        if self.poll_deadline(cx) { return Poll::Ready(Err(Self::timed_out())); }
        let result = Pin::new(&mut self.socket).poll_flush(cx);
        if result.is_ready() { self.deadline = None; }
        result
    }
    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> { Pin::new(&mut self.socket).poll_shutdown(cx) }
}
