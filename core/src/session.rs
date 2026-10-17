use crate::{bridge::ffi::{SessionEvent, SessionInfo}, model::{Effective, Protocol}};
use std::{sync::mpsc::{self, Receiver, SyncSender, TrySendError}, thread::{self, JoinHandle}, time::Duration};
use uuid::Uuid;

pub enum Command { Input(String), Resize(u32, u32) }

pub struct Session {
    pub id: String,
    pub profile: Option<String>,
    pub state: String,
    pub mock: bool,
    view_only: bool,
    commands: Option<SyncSender<Command>>,
    stop: Option<mpsc::Sender<()>>,
    events: Option<Receiver<SessionEvent>>,
    worker: Option<JoinHandle<()>>,
}

impl Session {
    pub fn new(profile: Option<&str>, title: &str, e: Effective, mock: bool) -> Result<(Self, SessionInfo), String> {
        if e.protocol == Protocol::Unsupported || !e.unsupported.is_empty() { return Err("Native connection settings are unsupported".into()); }
        if !mock {
            match e.protocol {
                Protocol::Rdp => crate::model::validate_rdp_target(&e.host, e.port, &e.username, &e.domain)?,
                Protocol::Ssh => crate::model::validate_ssh_target(&e.host, e.port, &e.username, &e.ssh_key_path)?,
                Protocol::Vnc => crate::model::validate_vnc_target(&e.host, e.port)?,
                _ => return Err("This protocol has no real backend in this milestone".into()),
            }
        }
        let options = (&e).into();
        let id = Uuid::new_v4().to_string();
        let info = SessionInfo { id: id.clone(), title: title.to_owned(), protocol: e.protocol.name().into(),
            host: e.host, port: e.port,
            username: if e.protocol == Protocol::Vnc { String::new() } else { e.username },
            domain: if e.protocol == Protocol::Vnc { String::new() } else { e.domain }, options,
            ssh_auth: e.ssh_auth.name().into(), ssh_key_path: e.ssh_key_path, view_only: e.protocol == Protocol::Vnc && e.view_only,
            mock, graphical: matches!(e.protocol, Protocol::Rdp | Protocol::Vnc),
            keyboard: e.protocol != Protocol::Vnc || !e.view_only,
            mouse: e.protocol != Protocol::Ssh && (e.protocol != Protocol::Vnc || !e.view_only),
            clipboard: !mock && e.protocol == Protocol::Rdp,
            resize: e.protocol != Protocol::Vnc };
        let mut s = Self { id: id.clone(), profile: profile.map(str::to_owned), state: "connecting".into(), mock, view_only: info.view_only,
            commands: None, stop: None, events: None, worker: None };
        if mock {
            let (tx, rx) = mpsc::sync_channel(128);
            let (etx, erx) = mpsc::sync_channel(32);
            let (stop_tx, stop_rx) = mpsc::channel();
            s.commands = Some(tx); s.events = Some(erx); s.stop = Some(stop_tx);
            s.worker = Some(thread::spawn(move || {
                let _ = etx.try_send(SessionEvent { id: id.clone(), kind: "state".into(), text: "connected".into(), width: 0, height: 0 });
                while stop_rx.try_recv().is_err() {
                    match rx.recv_timeout(Duration::from_millis(10)) {
                        Ok(command) => {
                            let (kind, text, width, height) = match command {
                                Command::Input(text) => ("input", text, 0, 0),
                                Command::Resize(w, h) => ("resize", format!("Mock viewport: {w} × {h}"), w, h),
                            };
                            let _ = etx.try_send(SessionEvent { id: id.clone(), kind: kind.into(), text, width, height });
                        }
                        Err(mpsc::RecvTimeoutError::Disconnected) => break,
                        Err(mpsc::RecvTimeoutError::Timeout) => {}
                    }
                }
            }));
        }
        Ok((s, info))
    }

    pub fn command(&self, command: Command) -> Result<(), String> {
        if self.view_only && matches!(&command, Command::Input(_)) { return Err("VNC session is view-only".into()); }
        let tx = self.commands.as_ref().ok_or("Not a mock session")?;
        tx.try_send(command).map_err(|e| match e {
            TrySendError::Full(_) => "Mock input queue is full".into(),
            TrySendError::Disconnected(_) => "Session is closed".into(),
        })
    }

    pub fn poll(&mut self) -> Vec<SessionEvent> {
        let events: Vec<_> = self.events.as_ref().map(|r| r.try_iter().collect()).unwrap_or_default();
        for event in &events { if event.kind == "state" { self.state = event.text.clone(); } }
        events
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        if let Some(stop) = self.stop.take() { let _ = stop.send(()); }
        self.commands.take();
        if let Some(worker) = self.worker.take() { let _ = worker.join(); }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Document, Kind, Profile};
    #[test]
    fn mock_resize_input_cancel_and_reopen() {
        let mut d = Document::default(); let p = Profile::new("Test", Kind::Connection, None);
        d.upsert(p.clone()).unwrap();
        let (mut s, first) = Session::new(Some(&p.id), &p.name, d.effective(&p.id).unwrap(), true).unwrap();
        s.command(Command::Resize(640, 480)).unwrap(); s.command(Command::Input("key 65".into())).unwrap();
        let mut events = Vec::new();
        for _ in 0..100 { events.extend(s.poll()); if events.len() >= 3 { break; } thread::sleep(Duration::from_millis(5)); }
        assert!(events.iter().any(|e| e.width == 640 && e.height == 480));
        assert!(events.iter().any(|e| e.text == "key 65"));
        drop(s);
        let (s, second) = Session::new(Some(&p.id), &p.name, d.effective(&p.id).unwrap(), true).unwrap();
        assert_ne!(first.id, second.id); drop(s);
    }
}
