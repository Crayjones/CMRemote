use crate::credentials::Secret;

#[path = "vnc_transport.rs"]
pub mod transport;
pub use transport::{Backend, BackendControl};

#[cxx::bridge(namespace = "rm::vnc")]
pub mod ffi {
    struct BackendTarget {
        host: String,
        port: u16,
        view_only: bool,
    }
    struct GraphicsEvent {
        kind: u8,
        x: u16,
        y: u16,
        width: u16,
        height: u16,
        src_x: u16,
        src_y: u16,
        pixels: Vec<u8>,
    }
    struct BackendEvent {
        phase: u8,
        error: String,
    }
    unsafe extern "C++" {
        include!("VncEventSink.h");
        type EventSink;
        fn graphics(self: &EventSink, event: &GraphicsEvent) -> bool;
        fn wake(self: &EventSink, generation: u64);
    }
    extern "Rust" {
        type Backend;
        type BackendControl;
        type Authentication;
        fn new_backend(target: BackendTarget, auth: Box<Authentication>, sink: SharedPtr<EventSink>, generation: u64) -> Result<Box<Backend>>;
        fn control(self: &Backend) -> Box<BackendControl>;
        fn run_backend(backend: Box<Backend>);
        fn key(self: &BackendControl, keysym: u32, down: bool) -> Result<()>;
        fn pointer(self: &BackendControl, mask: u8, x: u16, y: u16) -> Result<()>;
        fn stop(self: &BackendControl);
        fn resolved(self: &BackendControl, addresses: Vec<String>) -> bool;
        fn poll(self: &BackendControl) -> BackendEvent;
        fn password(self: &BackendControl, password: Vec<u8>) -> Result<()>;
        fn password_auth(password: Vec<u8>) -> Result<Box<Authentication>>;
        fn prompt_auth() -> Box<Authentication>;
    }
}

// Immutable callbacks and retained lifetime match SSH. Only the native owning
// worker invokes graphics; control clones can only queue input/DNS/cancellation.
unsafe impl Send for ffi::EventSink {}
unsafe impl Sync for ffi::EventSink {}
pub fn new_backend(target: ffi::BackendTarget, auth: Box<Authentication>, sink: cxx::SharedPtr<ffi::EventSink>, generation: u64) -> Result<Box<Backend>, Failure> {
    if sink.is_null() { return Err(Failure::Runtime); }
    let notify = sink.clone();
    let (_, backend) = transport::create(target, auth, move |event| sink.graphics(event), move || notify.wake(generation))?;
    Ok(Box::new(backend))
}
pub fn run_backend(backend: Box<Backend>) { backend.run(); }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Failure {
    AuthenticationBounds, Target, Resolve, Connect, Handshake, Rejected, Deadline,
    Cancelled, Runtime, Closed, ViewOnly, InputBounds, InputOverload, Bounds, Protocol,
}
impl std::fmt::Display for Failure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::AuthenticationBounds => "VNC passwords require 1–8 printable ASCII bytes.",
            Self::Target => "The VNC target is invalid or exceeds supported limits.",
            Self::Resolve => "The VNC hostname could not be resolved.",
            Self::Connect => "The VNC server could not be reached.",
            Self::Handshake => "The VNC handshake or requested authentication failed.",
            Self::Rejected => "The VNC server rejected the password.",
            Self::Deadline => "The VNC connection or write stage timed out.",
            Self::Cancelled => "VNC connection cancelled.",
            Self::Runtime => "The VNC worker could not start.",
            Self::Closed => "The VNC session no longer accepts input.",
            Self::ViewOnly => "This VNC session is view-only.",
            Self::InputBounds => "VNC input is outside the supported bounds.",
            Self::InputOverload => "The VNC input queue is full; the connection was stopped to preserve input transitions.",
            Self::Bounds => "VNC graphics exceed supported bounds.",
            Self::Protocol => "The VNC server sent invalid or unsupported data.",
        })
    }
}
impl std::error::Error for Failure {}

// Password: a saved password, sent only if the server selects VncAuth.
// Prompt: no saved password; ask the GUI only if the server requires one.
// Either way None is used whenever the server offers it, like mRemoteNG.
pub(super) enum AuthenticationKind { Password(Secret), Prompt }
pub struct Authentication { pub(super) kind: AuthenticationKind }
impl std::fmt::Debug for Authentication {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str("Authentication([REDACTED])") }
}
pub(super) fn valid_password(password: Vec<u8>) -> Result<Secret, Failure> {
    let password = Secret::new(password);
    if !(1..=8).contains(&password.expose().len()) || !password.expose().iter().all(|byte| (0x20..=0x7e).contains(byte)) {
        return Err(Failure::AuthenticationBounds);
    }
    Ok(password)
}
pub fn password_auth(password: Vec<u8>) -> Result<Box<Authentication>, Failure> {
    Ok(Box::new(Authentication { kind: AuthenticationKind::Password(valid_password(password)?) }))
}
pub fn prompt_auth() -> Box<Authentication> { Box::new(Authentication { kind: AuthenticationKind::Prompt }) }
