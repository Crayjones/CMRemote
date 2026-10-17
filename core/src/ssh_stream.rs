use super::Failure;
use std::{collections::VecDeque, sync::{Arc, Mutex, atomic::{AtomicBool, Ordering}}};
use tokio::sync::{Notify, watch};

pub const PACKET_BYTES: usize = 32 * 1024;
pub const INPUT_BYTES: usize = 256 * 1024;
pub const OUTPUT_BYTES: usize = 1024 * 1024;
pub const OUTPUT_ENTRIES: usize = 64;
pub const GUI_BYTES: usize = 128 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TerminalSize { pub rows: u16, pub columns: u16 }
pub enum StreamCommand { Input(Vec<u8>), Resize(TerminalSize) }
pub struct StreamDrain { pub packets: Vec<Vec<u8>>, pub more: bool, pub ended: bool }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StreamStats { pub input_bytes: usize, pub output_bytes: usize, pub output_entries: usize }

struct Input { bytes: VecDeque<u8>, resize: Option<TerminalSize> }
struct Output { packets: VecDeque<Vec<u8>>, bytes: usize, wake_pending: bool }
struct Shared {
    input: Mutex<Input>, output: Mutex<Output>,
    input_ready: Notify, output_credit: Notify,
    stop: watch::Sender<bool>, stopped: AtomicBool, finished: AtomicBool,
    wake: Box<dyn Fn() + Send + Sync>,
}
#[derive(Clone)]
pub struct StreamControl { shared: Arc<Shared> }
// Exactly one producer and one input consumer. No Core or GUI borrow.
pub struct StreamWorker { control: StreamControl, commands: Option<StreamCommands> }
pub struct StreamCommands { control: StreamControl }

// Wake must queue a nonthrowing, nonblocking notification, not access GUI objects.
// Each wake permits one GUI drain. A partial drain queues the next wake itself.
pub fn byte_stream(wake: impl Fn() + Send + Sync + 'static) -> (StreamControl, StreamWorker) {
    let (stop, _) = watch::channel(false);
    let control = StreamControl { shared: Arc::new(Shared {
        input: Mutex::new(Input { bytes: VecDeque::with_capacity(INPUT_BYTES), resize: None }),
        output: Mutex::new(Output { packets: VecDeque::with_capacity(OUTPUT_ENTRIES), bytes: 0, wake_pending: false }),
        input_ready: Notify::new(), output_credit: Notify::new(),
        stop, stopped: AtomicBool::new(false), finished: AtomicBool::new(false), wake: Box::new(wake),
    }) };
    let worker = StreamWorker { control: control.clone(), commands: Some(StreamCommands { control: control.clone() }) };
    (control, worker)
}
fn bounds(bytes: &[u8]) -> Result<(), Failure> {
    if bytes.is_empty() || bytes.len() > PACKET_BYTES { Err(Failure::StreamBounds) } else { Ok(()) }
}
impl Shared {
    fn active(&self) -> Result<(), Failure> {
        if self.stopped.load(Ordering::Acquire) { Err(Failure::Cancelled) }
        else if self.finished.load(Ordering::Acquire) { Err(Failure::StreamClosed) }
        else { Ok(()) }
    }
}
impl StreamControl {
    pub fn input(&self, bytes: &[u8]) -> Result<(), Failure> {
        bounds(bytes)?;
        let mut input = self.shared.input.lock().unwrap(); self.shared.active()?;
        if bytes.len() > INPUT_BYTES - input.bytes.len() { return Err(Failure::InputOverload); }
        input.bytes.extend(bytes.iter().copied()); drop(input);
        self.shared.input_ready.notify_one(); Ok(())
    }
    pub fn resize(&self, rows: u16, columns: u16) -> Result<(), Failure> {
        if !(1..=256).contains(&rows) || !(1..=512).contains(&columns) { return Err(Failure::TerminalSize); }
        let mut input = self.shared.input.lock().unwrap(); self.shared.active()?;
        input.resize = Some(TerminalSize { rows, columns }); drop(input);
        self.shared.input_ready.notify_one(); Ok(())
    }
    // Independent of queue capacity and queue mutexes; never wait for the worker.
    pub fn stop(&self) {
        if !self.shared.stopped.swap(true, Ordering::AcqRel) { self.shared.stop.send_replace(true); }
    }
    pub fn is_stopped(&self) -> bool { self.shared.stopped.load(Ordering::Acquire) }
    pub async fn cancelled(&self) {
        let mut observe = self.shared.stop.subscribe();
        let stopped = *observe.borrow_and_update();
        if !stopped { let _ = observe.changed().await; }
    }
    pub fn drain(&self) -> StreamDrain {
        let mut output = self.shared.output.lock().unwrap();
        if self.is_stopped() { output.packets.clear(); output.bytes = 0; }
        let mut packets = Vec::with_capacity(OUTPUT_ENTRIES); let mut bytes = 0;
        while output.packets.front().is_some_and(|packet| packet.len() <= GUI_BYTES - bytes) {
            let packet = output.packets.pop_front().unwrap();
            bytes += packet.len(); output.bytes -= packet.len(); packets.push(packet);
        }
        let more = !output.packets.is_empty(); output.wake_pending = more;
        let ended = self.shared.finished.load(Ordering::Acquire) && !more;
        drop(output);
        self.shared.output_credit.notify_one();
        if more { (self.shared.wake)(); }
        StreamDrain { packets, more, ended }
    }
    pub fn stats(&self) -> StreamStats {
        let input_bytes = self.shared.input.lock().unwrap().bytes.len();
        let output = self.shared.output.lock().unwrap();
        StreamStats { input_bytes, output_bytes: output.bytes, output_entries: output.packets.len() }
    }
}
impl StreamCommands {
    pub async fn next_command(&mut self) -> Result<StreamCommand, Failure> {
        let shared = &self.control.shared;
        loop {
            let ready = shared.input_ready.notified();
            {
                let mut input = shared.input.lock().unwrap(); shared.active()?;
                if let Some(size) = input.resize.take() { return Ok(StreamCommand::Resize(size)); }
                let length = input.bytes.len().min(PACKET_BYTES);
                if length > 0 { return Ok(StreamCommand::Input(input.bytes.drain(..length).collect())); }
            }
            tokio::select! {
                biased;
                _ = self.control.cancelled() => return Err(Failure::Cancelled),
                _ = ready => {},
            }
        }
    }
}
impl StreamWorker {
    pub fn take_commands(&mut self) -> Option<StreamCommands> { self.commands.take() }
    pub async fn next_command(&mut self) -> Result<StreamCommand, Failure> {
        self.commands.as_mut().ok_or(Failure::StreamClosed)?.next_command().await
    }
    pub async fn output(&mut self, bytes: Vec<u8>) -> Result<(), Failure> {
        bounds(&bytes)?;
        // Do not retain excess producer allocation behind a small byte count.
        let bytes = bytes.into_boxed_slice().into_vec();
        let shared = &self.control.shared;
        loop {
            let credit = shared.output_credit.notified();
            {
                let mut output = shared.output.lock().unwrap(); shared.active()?;
                if output.packets.len() < OUTPUT_ENTRIES && bytes.len() <= OUTPUT_BYTES - output.bytes {
                    let wake = !output.wake_pending; output.wake_pending = true;
                    output.bytes += bytes.len(); output.packets.push_back(bytes); drop(output);
                    if wake { (shared.wake)(); }
                    return Ok(());
                }
            }
            tokio::select! {
                biased;
                _ = self.control.cancelled() => return Err(Failure::Cancelled),
                _ = credit => {},
            }
        }
    }
    // EOF retains accepted output. Cancellation discards retired output instead.
    // End-of-stream is visible only after the last retained packet is drained.
    pub fn finish(&mut self) {
        let shared = &self.control.shared;
        if shared.finished.swap(true, Ordering::AcqRel) { return; }
        {
            let mut input = shared.input.lock().unwrap(); input.bytes.clear(); input.resize = None;
        }
        shared.input_ready.notify_one();
        let mut output = shared.output.lock().unwrap();
        if self.control.is_stopped() { output.packets.clear(); output.bytes = 0; }
        let wake = !output.wake_pending; output.wake_pending = true; drop(output);
        if wake { (shared.wake)(); }
    }
}
impl Drop for StreamWorker { fn drop(&mut self) { self.finish(); } }
