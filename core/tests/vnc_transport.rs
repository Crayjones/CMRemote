use remotemanager_core::vnc::{self, Failure, ffi::{BackendTarget, GraphicsEvent}, transport::{self, BackendControl, Timeouts}};
use ::vnc as vnc_engine;
use std::{io::{Read, Write}, net::{TcpListener, TcpStream}, sync::{Arc, Mutex, mpsc}, thread, time::{Duration, Instant}};

const WAIT: Duration = Duration::from_secs(5);
const BGRA: [u8; 16] = [32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0];
#[derive(Clone, Debug)]
struct Graphics { kind: u8, x: u16, y: u16, width: u16, height: u16, src_x: u16, src_y: u16, pixels: Vec<u8> }
impl From<&GraphicsEvent> for Graphics {
    fn from(e: &GraphicsEvent) -> Self { Self { kind: e.kind, x: e.x, y: e.y, width: e.width, height: e.height, src_x: e.src_x, src_y: e.src_y, pixels: e.pixels.clone() } }
}
struct Worker { control: BackendControl, thread: Option<thread::JoinHandle<()>>, done: mpsc::Receiver<()>, graphics: Arc<Mutex<Vec<Graphics>>>, commits: Arc<Mutex<usize>> }
impl Worker {
    fn start(port: u16, auth: Box<vnc::Authentication>, view_only: bool) -> Self {
        Self::start_with(port, auth, view_only, Timeouts::default(), |_| true)
    }
    fn start_with(port: u16, auth: Box<vnc::Authentication>, view_only: bool, timeouts: Timeouts, accept: impl Fn(&GraphicsEvent) -> bool + Send + 'static) -> Self {
        let graphics = Arc::new(Mutex::new(Vec::new()));
        let collector = graphics.clone();
        let commits = Arc::new(Mutex::new(0_usize));
        let committed = commits.clone();
        let (control, backend) = transport::create_with_timeouts(BackendTarget { host: "localhost".into(), port, view_only }, auth, move |e| {
            // Update-boundary commits are counted separately from rectangle events.
            if e.kind == 4 { assert!(e.pixels.is_empty() && e.width == 0 && e.height == 0); *commits.lock().unwrap() += 1; return true; }
            if !accept(e) { return false; }
            collector.lock().unwrap().push(e.into()); true
        }, || {}, timeouts).unwrap();
        let (send, done) = mpsc::channel();
        let thread = thread::spawn(move || { backend.run(); send.send(()).unwrap(); });
        assert!(control.resolved(vec!["127.0.0.1".into()]));
        Self { control, thread: Some(thread), done, graphics, commits: committed }
    }
    fn phase(&self, phase: u8) {
        wait(|| { let e = self.control.poll(); assert!(e.phase != 6 || phase == 6, "unexpected failure: {}", e.error); e.phase == phase });
    }
    fn events(&self, count: usize) -> Vec<Graphics> {
        wait(|| self.graphics.lock().unwrap().len() >= count);
        self.graphics.lock().unwrap().clone()
    }
    fn joined(&mut self) {
        if let Some(thread) = self.thread.take() {
            self.done.recv_timeout(WAIT).expect("native runtime did not retire");
            thread.join().unwrap();
        }
    }
}
impl Drop for Worker { fn drop(&mut self) { self.control.stop(); self.joined(); } }
fn wait(mut condition: impl FnMut() -> bool) {
    let limit = Instant::now() + WAIT;
    while !condition() { assert!(Instant::now() < limit, "condition timed out"); thread::sleep(Duration::from_millis(2)); }
}
fn fixture(run: impl FnOnce(TcpStream) + Send + 'static) -> (u16, thread::JoinHandle<()>) {
    let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
    let port = listener.local_addr().unwrap().port();
    (port, thread::spawn(move || {
        let (stream, _) = listener.accept().unwrap();
        stream.set_read_timeout(Some(WAIT)).unwrap(); stream.set_write_timeout(Some(WAIT)).unwrap(); stream.set_nodelay(true).unwrap();
        run(stream);
    }))
}
fn bytes<const N: usize>(s: &mut TcpStream) -> [u8; N] { let mut out = [0; N]; s.read_exact(&mut out).unwrap(); out }
fn greeting(s: &mut TcpStream, version: u8) {
    let greeting = format!("RFB 003.00{version}\n");
    // Fragment the negotiation to exercise async read_exact, not packet assumptions.
    for byte in greeting.as_bytes() { s.write_all(&[*byte]).unwrap(); }
    assert_eq!(bytes::<12>(s), greeting.as_bytes());
}
fn security(s: &mut TcpStream, version: u8, offers: &[u8], selected: u8) {
    greeting(s, version);
    if version == 3 { s.write_all(&u32::from(selected).to_be_bytes()).unwrap(); }
    else { s.write_all(&[offers.len() as u8]).unwrap(); s.write_all(offers).unwrap(); assert_eq!(bytes::<1>(s), [selected]); }
    if selected == 2 {
        s.write_all(&(0..16).collect::<Vec<u8>>()).unwrap();
        // Independent DES/ECB reference: reversed password bits, no padding.
        assert_eq!(bytes::<16>(s), [0xb8,0x66,0x92,0x41,0x25,0xc8,0xee,0xbb,0x9d,0xeb,0xc1,0xdb,0x61,0xc5,0x38,0xe2]);
        s.write_all(&0_u32.to_be_bytes()).unwrap();
    } else if version == 8 { s.write_all(&0_u32.to_be_bytes()).unwrap(); }
}
fn init(s: &mut TcpStream, width: u16, height: u16) {
    assert_eq!(bytes::<1>(s), [1]); // Shared ClientInit.
    s.write_all(&width.to_be_bytes()).unwrap(); s.write_all(&height.to_be_bytes()).unwrap();
    s.write_all(&BGRA).unwrap(); s.write_all(&4_u32.to_be_bytes()).unwrap(); s.write_all(b"test").unwrap();
    let format = bytes::<20>(s); assert_eq!(&format[..4], &[0; 4]); assert_eq!(&format[4..], &BGRA);
    assert_eq!(bytes::<2>(s), [2, 0]); let count = u16::from_be_bytes(bytes(s));
    let encodings: Vec<i32> = (0..count).map(|_| i32::from_be_bytes(bytes(s))).collect();
    // ZRLE, Tight, TRLE, CopyRect, Raw, LastRect; no JPEG, resize or clipboard extensions.
    assert_eq!(encodings, [16, 7, 15, 1, 0, -224, -239, -223]);
    assert_eq!(bytes::<10>(s), [3, 0, 0, 0, 0, 0, (width >> 8) as u8, width as u8, (height >> 8) as u8, height as u8]);
}
fn handshake(s: &mut TcpStream) { security(s, 8, &[1], 1); init(s, 4, 3); }
fn update(s: &mut TcpStream, rects: u16) { s.write_all(&[0, 0]).unwrap(); s.write_all(&rects.to_be_bytes()).unwrap(); }
fn rect(s: &mut TcpStream, x: u16, y: u16, w: u16, h: u16, encoding: i32) {
    for value in [x, y, w, h] { s.write_all(&value.to_be_bytes()).unwrap(); }
    s.write_all(&encoding.to_be_bytes()).unwrap();
}
fn incremental(s: &mut TcpStream, w: u16, h: u16) { assert_eq!(bytes::<10>(s), [3, 1, 0, 0, 0, 0, (w >> 8) as u8, w as u8, (h >> 8) as u8, h as u8]); }
fn eof(s: &mut TcpStream) { let mut rest = Vec::new(); s.read_to_end(&mut rest).unwrap(); }

#[test]
fn password_bounds_and_redaction() {
    for password in [vec![], vec![b'a'; 9], vec![0], vec![0x1f], vec![0x7f], "é".as_bytes().to_vec()] {
        assert_eq!(vnc::password_auth(password).unwrap_err(), Failure::AuthenticationBounds);
    }
    for password in [b" ".to_vec(), b"password".to_vec(), b"~".to_vec()] {
        let auth = vnc::password_auth(password.clone()).unwrap();
        assert!(!format!("{auth:?}").contains(std::str::from_utf8(&password).unwrap()));
    }
}
#[test]
fn classic_auth_vectors_all_versions_with_saved_password() {
    for version in [3, 7, 8] {
        let (port, server) = fixture(move |mut s| { security(&mut s, version, &[250, 2], 2); init(&mut s, 4, 3); eof(&mut s); });
        let worker = Worker::start(port, vnc::password_auth(b"password".to_vec()).unwrap(), false);
        worker.phase(3); assert_eq!(worker.events(1)[0].kind, 0);
        drop(worker); server.join().unwrap();
    }
}
#[test]
fn none_is_selected_whenever_offered_and_no_password_is_sent() {
    for version in [3, 7, 8] {
        // Mixed offers (and RFB 3.3's server-chosen None) never send a saved password.
        let (port, server) = fixture(move |mut s| { security(&mut s, version, &[1, 250, 2], 1); init(&mut s, 4, 3); eof(&mut s); });
        let worker = Worker::start(port, vnc::password_auth(b"password".to_vec()).unwrap(), false);
        worker.phase(3); drop(worker); server.join().unwrap();
        let (port, server) = fixture(move |mut s| { security(&mut s, version, &[1], 1); init(&mut s, 4, 3); eof(&mut s); });
        let worker = Worker::start(port, vnc::prompt_auth(), false); worker.phase(3); drop(worker); server.join().unwrap();
    }
}
#[test]
fn password_prompt_waits_without_stage_deadline_and_rejects_invalid_input() {
    for version in [3, 7, 8] {
        let (port, server) = fixture(move |mut s| { security(&mut s, version, &[2], 2); init(&mut s, 4, 3); eof(&mut s); });
        let limits = Timeouts { stage: Duration::from_millis(300), address: WAIT, write: WAIT };
        let worker = Worker::start_with(port, vnc::prompt_auth(), false, limits, |_| true);
        worker.phase(7);
        // A user typing for longer than the stage deadline must not fail the handshake.
        thread::sleep(Duration::from_millis(700)); assert_eq!(worker.control.poll().phase, 7);
        assert_eq!(worker.control.password(b"too-long-password".to_vec()), Err(Failure::AuthenticationBounds));
        assert_eq!(worker.control.password(Vec::new()), Err(Failure::AuthenticationBounds));
        assert_eq!(worker.control.poll().phase, 7, "invalid input keeps the request open");
        worker.control.password(b"password".to_vec()).unwrap();
        assert_eq!(worker.control.password(b"password".to_vec()), Err(Failure::Closed), "one answer per request");
        worker.phase(3); drop(worker); server.join().unwrap();
    }
    // Cancelling while the prompt is open retires promptly as Cancelled.
    let (port, server) = fixture(move |mut s| { greeting(&mut s, 8); s.write_all(&[1, 2]).unwrap(); eof(&mut s); });
    let mut worker = Worker::start(port, vnc::prompt_auth(), false);
    worker.phase(7); worker.control.stop(); worker.joined(); assert_eq!(worker.control.poll().phase, 5);
    assert_eq!(worker.control.password(b"password".to_vec()), Err(Failure::Closed)); server.join().unwrap();
}
#[test]
fn rejected_passwords_are_reported_without_server_text() {
    for version in [3, 7, 8] {
        let (port, server) = fixture(move |mut s| {
            greeting(&mut s, version);
            if version == 3 { s.write_all(&2_u32.to_be_bytes()).unwrap(); } else { s.write_all(&[1, 2]).unwrap(); assert_eq!(bytes::<1>(&mut s), [2]); }
            s.write_all(&(0..16).collect::<Vec<u8>>()).unwrap(); bytes::<16>(&mut s);
            s.write_all(&1_u32.to_be_bytes()).unwrap();
            if version == 8 { s.write_all(&19_u32.to_be_bytes()).unwrap(); s.write_all(b"PRIVATE_SERVER_TEXT").unwrap(); }
            eof(&mut s);
        });
        let mut worker = Worker::start(port, vnc::password_auth(b"wrong".to_vec()).unwrap(), false);
        worker.phase(6); let error = worker.control.poll().error;
        assert_eq!(error, Failure::Rejected.to_string()); assert!(!error.contains("PRIVATE")); worker.joined(); server.join().unwrap();
    }
}
#[test]
fn incompatible_security_and_server_errors_are_generic() {
    for response in [vec![1, 19], vec![1, 250], { let mut r = vec![0]; r.extend(19_u32.to_be_bytes()); r.extend(b"PRIVATE_SERVER_TEXT"); r }] {
        let (port, server) = fixture(move |mut s| { greeting(&mut s, 8); s.write_all(&response).unwrap(); eof(&mut s); });
        let mut worker = Worker::start(port, vnc::password_auth(b"password".to_vec()).unwrap(), false);
        worker.phase(6); let error = worker.control.poll().error;
        assert_eq!(error, Failure::Handshake.to_string()); assert!(!error.contains("PRIVATE")); worker.joined(); server.join().unwrap();
    }
}
#[test]
fn empty_updates_pace_one_outstanding_request_and_idle_input_stays_responsive() {
    let (sent, ready) = mpsc::channel();
    let (port, server) = fixture(move |mut s| {
        handshake(&mut s);
        s.set_read_timeout(Some(Duration::from_millis(60))).unwrap();
        let mut b = [0]; assert!(s.read(&mut b).is_err(), "duplicate initial full request");
        s.set_read_timeout(Some(WAIT)).unwrap();
        update(&mut s, 0); let start = Instant::now(); incremental(&mut s, 4, 3);
        assert!(start.elapsed() >= Duration::from_millis(10), "empty update hot loop");
        // This pending incremental is legitimately idle and must not block input.
        sent.send(()).unwrap();
        assert_eq!(bytes::<8>(&mut s), [4, 1, 0, 0, 0, 0, 0, 0x61]);
        assert_eq!(bytes::<8>(&mut s), [4, 0, 0, 0, 0, 0, 0, 0x61]);
        assert_eq!(bytes::<6>(&mut s), [5, 1, 0, 2, 0, 1]);
        sent.send(()).unwrap(); eof(&mut s);
    });
    let worker = Worker::start_with(port, vnc::prompt_auth(), false, Timeouts { stage: Duration::from_millis(300), address: WAIT, write: WAIT }, |_| true);
    worker.phase(3); ready.recv_timeout(WAIT).unwrap();
    // Idle longer than connection stage limit does not fail an established session.
    thread::sleep(Duration::from_millis(400)); assert_eq!(worker.control.poll().phase, 3);
    worker.control.key(0x61, true).unwrap(); worker.control.key(0x61, false).unwrap(); worker.control.pointer(1, 2, 1).unwrap();
    ready.recv_timeout(WAIT).unwrap(); drop(worker); server.join().unwrap();
}
#[test]
fn raw_copy_cursor_and_server_resize_decode_to_worker_sink() {
    let (sent, ready) = mpsc::channel();
    let (port, server) = fixture(move |mut s| {
        handshake(&mut s); update(&mut s, 5);
        rect(&mut s, 1, 0, 2, 1, 0); s.write_all(&[1, 2, 3, 0, 4, 5, 6, 7]).unwrap();
        rect(&mut s, 0, 1, 2, 1, 1); s.write_all(&[0, 1, 0, 0]).unwrap();
        rect(&mut s, 0, 0, 2, 1, -239); s.write_all(&[9, 8, 7, 0, 6, 5, 4, 200, 0x80]).unwrap();
        rect(&mut s, 0, 0, 0, 0, -239);
        rect(&mut s, 0, 0, 5, 4, -223);
        incremental(&mut s, 5, 4); sent.send(()).unwrap(); eof(&mut s);
    });
    let worker = Worker::start(port, vnc::prompt_auth(), false); ready.recv_timeout(WAIT).unwrap();
    let events = worker.events(6);
    assert_eq!(events.iter().map(|e| e.kind).collect::<Vec<_>>(), [0, 1, 2, 3, 3, 0]);
    // The next request is only sent after UpdateComplete, which commits first.
    assert_eq!(*worker.commits.lock().unwrap(), 1);
    let raw = &events[1]; assert_eq!((raw.x, raw.y, raw.width, raw.height), (1, 0, 2, 1)); assert_eq!(raw.pixels, [1,2,3,0,4,5,6,7]);
    let copy = &events[2]; assert_eq!((copy.x, copy.y, copy.width, copy.height, copy.src_x, copy.src_y), (0,1,2,1,1,0));
    assert_eq!(events[3].pixels, [9,8,7,255,6,5,4,0]); assert_eq!((events[4].width, events[4].height), (0,0));
    assert_eq!((events[5].width, events[5].height), (5,4)); drop(worker); server.join().unwrap();
}
#[test]
fn zrle_streams_bounded_tiles_and_rejects_oversized_runs() {
    // Python zlib reference streams with Z_SYNC_FLUSH. First is raw CPixels;
    // second encodes a true-color run larger than its 2-pixel destination tile.
    for (compressed, valid) in [
        (vec![0x78,0x9c,0x62,0x60,0x64,0x62,0x66,0x61,0x65,0x03,0,0,0,0xff,0xff], true),
        (vec![0x78,0x9c,0x6a,0x60,0x64,0x62,0xfe,0xff,0x9f,0x11,0,0,0,0xff,0xff], false),
    ] {
        let (sent, ready) = mpsc::channel();
        let (port, server) = fixture(move |mut s| {
            handshake(&mut s); update(&mut s, 1); rect(&mut s, 1, 1, 2, 1, 16);
            s.write_all(&(compressed.len() as u32).to_be_bytes()).unwrap(); s.write_all(&compressed).unwrap();
            if valid { incremental(&mut s, 4, 3); sent.send(()).unwrap(); }
            eof(&mut s);
        });
        let mut worker = Worker::start(port, vnc::prompt_auth(), false);
        if valid {
            ready.recv_timeout(WAIT).unwrap();
            let events = worker.events(2); assert_eq!(events[1].pixels, [1,2,3,255,4,5,6,255]);
            assert_eq!((events[1].x, events[1].y, events[1].width, events[1].height), (1,1,2,1));
            worker.control.stop();
        } else { worker.phase(6); assert_eq!(worker.events(1).len(), 1); }
        worker.joined(); server.join().unwrap();
    }
}
// Independent Python zlib reference, Z_SYNC_FLUSH (not a terminated stream).
// 30x17 ZRLE solid tiles, cycling BGRA colors by (tile_x + tile_y) % 4.
const FULL_HD_ZRLE: &[u8] = &[
    0x78,0x9c,0x62,0x0c,0x30,0x78,0xc0,0x58,0x70,0x40,0x81,0xf1,0x41,0x82,0x01,
    0xa3,0xc1,0x81,0x07,0x8c,0xf4,0xe2,0x0f,0x84,0x9d,0x20,0xfe,0x40,0xd8,0x09,
    0xe2,0x0f,0x84,0x9d,0x20,0xfe,0x68,0xfc,0x8e,0xc6,0xef,0x68,0xfc,0x8e,0xc6,
    0xef,0x68,0xfc,0x8e,0xc6,0xef,0x68,0xfc,0x8e,0xc6,0x2f,0x12,0x1f,0,0,0,0xff,0xff,
];
fn full_hd_update(s: &mut TcpStream) {
    update(s, 1); rect(s, 0, 0, 1920, 1080, 16);
    s.write_all(&(FULL_HD_ZRLE.len() as u32).to_be_bytes()).unwrap();
    s.write_all(FULL_HD_ZRLE).unwrap();
}
#[test]
fn zrle_tile_burst_drains_without_per_tile_poll_delay() {
    let (sent, ready) = mpsc::channel();
    let (port, server) = fixture(move |mut s| {
        security(&mut s, 8, &[1], 1); init(&mut s, 1920, 1080);
        let start = Instant::now(); full_hd_update(&mut s);
        // This request proves UpdateComplete was consumed after all 510 tiles.
        incremental(&mut s, 1920, 1080); sent.send(start.elapsed()).unwrap(); eof(&mut s);
    });
    let worker = Worker::start(port, vnc::prompt_auth(), false);
    let elapsed = ready.recv_timeout(WAIT).unwrap();
    let events = worker.events(511);
    drop(worker); server.join().unwrap();
    assert_eq!(events.len(), 511);
    assert_eq!(events[0].kind, 0);
    let colors = [[80,48,224,255], [112,192,32,255], [224,96,48,255], [48,192,224,255]];
    for (index, tile) in events[1..].iter().enumerate() {
        let (x, y) = (index % 30, index / 30);
        let height = if y == 16 { 56 } else { 64 };
        assert_eq!((tile.kind, tile.x, tile.y, tile.width, tile.height), (1, (x * 64) as u16, (y * 64) as u16, 64, height));
        assert_eq!(tile.pixels.len(), 64 * usize::from(height) * 4);
        assert!(tile.pixels.chunks_exact(4).all(|pixel| pixel == colors[(x + y) % 4]));
    }
    eprintln!("510-tile ZRLE update through next request: {elapsed:?}");
    // Includes the intentional 16 ms refresh interval. A wide 500 ms ceiling
    // distinguishes scheduling stalls, not hardware-specific rendering speed.
    assert!(elapsed < Duration::from_millis(500), "tile delivery incurred polling delays: {elapsed:?}");
}

#[test]
fn input_transitions_reach_server_during_zrle_tile_burst() {
    let (first_tile, started) = mpsc::channel();
    let (resume, gate) = mpsc::channel();
    let (acknowledge, acknowledged) = mpsc::channel();
    let (finished, done) = mpsc::channel();
    let (port, server) = fixture(move |mut s| {
        security(&mut s, 8, &[1], 1); init(&mut s, 1920, 1080); full_hd_update(&mut s);
        assert_eq!(bytes::<8>(&mut s), [4, 1, 0, 0, 0, 0, 0, 0x61]);
        assert_eq!(bytes::<8>(&mut s), [4, 0, 0, 0, 0, 0, 0, 0x61]);
        assert_eq!(bytes::<6>(&mut s), [5, 1, 0, 2, 0, 1]);
        assert_eq!(bytes::<6>(&mut s), [5, 0, 0, 2, 0, 1]);
        acknowledge.send(()).unwrap();
        incremental(&mut s, 1920, 1080); finished.send(()).unwrap(); eof(&mut s);
    });
    let worker = Worker::start_with(port, vnc::prompt_auth(), false, Timeouts::default(), move |event| {
        if event.kind == 1 && event.x == 0 && event.y == 0 {
            first_tile.send(()).unwrap(); gate.recv_timeout(WAIT).unwrap();
        }
        if event.kind == 1 && event.x == 0 && event.y == 512 {
            // Deterministic checkpoint with over half the update left to process:
            // all four input transitions must already be acknowledged by the server.
            acknowledged.recv_timeout(Duration::from_secs(1)).expect("input starved behind decoder work");
        }
        true
    });
    started.recv_timeout(WAIT).unwrap();
    worker.control.key(0x61, true).unwrap(); worker.control.key(0x61, false).unwrap();
    worker.control.pointer(1, 2, 1).unwrap(); worker.control.pointer(0, 2, 1).unwrap();
    resume.send(()).unwrap(); done.recv_timeout(WAIT).unwrap();
    assert_eq!(worker.events(511).len(), 511);
    drop(worker); server.join().unwrap();
}
#[test]
fn cancellation_interrupts_zrle_tile_burst() {
    let (first_tile, started) = mpsc::channel(); let (resume, gate) = mpsc::channel();
    let (port, server) = fixture(move |mut s| {
        security(&mut s, 8, &[1], 1); init(&mut s, 1920, 1080); full_hd_update(&mut s); eof(&mut s);
    });
    let mut worker = Worker::start_with(port, vnc::prompt_auth(), false, Timeouts::default(), move |event| {
        if event.kind == 1 && event.x == 0 && event.y == 0 {
            first_tile.send(()).unwrap(); gate.recv_timeout(WAIT).unwrap();
        }
        true
    });
    started.recv_timeout(WAIT).unwrap();
    let start = Instant::now(); worker.control.stop(); resume.send(()).unwrap(); worker.joined();
    assert!(start.elapsed() < Duration::from_millis(500), "cancel waited for the full frame");
    assert_eq!(worker.control.poll().phase, 5);
    assert!(worker.events(2).len() < 511, "cancel did not interrupt the update");
    server.join().unwrap();
}

#[tokio::test]
async fn library_default_auth_policy_rejects_none_without_opt_in() {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let (client, mut server) = tokio::io::duplex(128);
    server.write_all(b"RFB 003.008\n\x01\x01").await.unwrap();
    let result = tokio::time::timeout(WAIT, vnc_engine::VncConnector::new(client)
        .set_auth_method(async { Ok("password".to_owned()) })
        .add_encoding(vnc_engine::VncEncoding::Raw).build().unwrap().try_start()).await.unwrap();
    assert!(matches!(result, Err(vnc_engine::VncError::ConnectError)));
    let mut sent = Vec::new(); server.read_to_end(&mut sent).await.unwrap(); assert_eq!(sent, b"RFB 003.008\n");
}
#[test]
fn invalid_graphics_fail_without_delivering_bad_mutations() {
    for (encoding, x, y, w, h, payload) in [
        (0, 3, 0, 2, 1, vec![]),
        (1, 0, 0, 2, 1, vec![0, 3, 0, 0]),
        (-223, 0, 0, 8193, 1, vec![]),
        (-223, 0, 0, 4096, 4096, vec![]),
        (-239, 1, 0, 1, 1, vec![1, 2, 3, 0, 0x80]),
        (5, 0, 0, 1, 1, vec![]), // Hextile is not advertised.
        (16, 0, 0, 1, 1, u32::MAX.to_be_bytes().to_vec()),
    ] {
        let (port, server) = fixture(move |mut s| { handshake(&mut s); update(&mut s, 1); rect(&mut s, x, y, w, h, encoding); s.write_all(&payload).unwrap(); eof(&mut s); });
        let mut worker = Worker::start(port, vnc::prompt_auth(), false); worker.phase(6); assert_eq!(worker.events(1).len(), 1); worker.joined(); server.join().unwrap();
    }
}
#[test]
fn sink_rejection_fails_closed_and_view_only_blocks_backend_input() {
    let (port, server) = fixture(move |mut s| { handshake(&mut s); update(&mut s, 1); rect(&mut s, 0, 0, 1, 1, 0); s.write_all(&[0; 4]).unwrap(); eof(&mut s); });
    let mut worker = Worker::start_with(port, vnc::prompt_auth(), false, Timeouts::default(), |event| event.kind == 0);
    worker.phase(6); assert_eq!(worker.control.poll().error, Failure::Bounds.to_string()); worker.joined(); server.join().unwrap();
    let (port, server) = fixture(move |mut s| { handshake(&mut s); eof(&mut s); });
    let worker = Worker::start(port, vnc::prompt_auth(), true); worker.phase(3);
    assert_eq!(worker.control.key(0x61, true), Err(Failure::ViewOnly)); assert_eq!(worker.control.pointer(1, 1, 1), Err(Failure::ViewOnly));
    drop(worker); server.join().unwrap();
}
#[test]
fn resolve_handshake_deadlines_and_cancellation_are_independent_of_queues() {
    let limits = Timeouts { stage: Duration::from_millis(40), address: Duration::from_millis(40), write: Duration::from_millis(40) };
    let (control, backend) = transport::create_with_timeouts(BackendTarget { host: "localhost".into(), port: 5900, view_only: false }, vnc::prompt_auth(), |_| true, || {}, limits).unwrap();
    let thread = thread::spawn(move || backend.run()); wait(|| control.poll().phase == 6); thread.join().unwrap(); assert_eq!(control.poll().error, Failure::Deadline.to_string());
    let (port, server) = fixture(move |mut s| { eof(&mut s); });
    let mut worker = Worker::start_with(port, vnc::prompt_auth(), false, limits, |_| true); worker.phase(6); worker.joined(); assert_eq!(worker.control.poll().error, Failure::Deadline.to_string()); server.join().unwrap();
    let (control, backend) = transport::create(BackendTarget { host: "localhost".into(), port: 5900, view_only: false }, vnc::prompt_auth(), |_| true, || {}).unwrap();
    control.stop(); let thread = thread::spawn(move || backend.run()); thread.join().unwrap(); assert_eq!(control.poll().phase, 5); assert!(!control.resolved(vec!["127.0.0.1".into()]));
}
#[test]
fn cancellation_retires_saturated_decoder_and_network_queues() {
    let (sent, ready) = mpsc::channel();
    let (port, server) = fixture(move |mut s| {
        handshake(&mut s); update(&mut s, 4000); sent.send(()).unwrap();
        let mut frame = Vec::new();
        for value in [0_u16, 0, 4, 3] { frame.extend(value.to_be_bytes()); } frame.extend(0_i32.to_be_bytes()); frame.extend([0_u8; 48]);
        for _ in 0..4000 { if s.write_all(&frame).is_err() { break; } }
        let _ = s.read(&mut [0]);
    });
    let mut worker = Worker::start_with(port, vnc::prompt_auth(), false, Timeouts::default(), |e| { if e.kind == 1 { thread::sleep(Duration::from_millis(1)); } true });
    ready.recv_timeout(WAIT).unwrap(); worker.events(3); worker.control.stop(); worker.joined(); assert_eq!(worker.control.poll().phase, 5); server.join().unwrap();
}
#[test]
fn input_pressure_fails_closed_without_dropping_transitions() {
    let (sent, ready) = mpsc::channel(); let (release, held) = mpsc::channel();
    let blocked = Arc::new(Mutex::new(Some(held))); let gate = blocked.clone();
    let (port, server) = fixture(move |mut s| { handshake(&mut s); update(&mut s, 1); rect(&mut s, 0, 0, 1, 1, 0); s.write_all(&[0; 4]).unwrap(); eof(&mut s); });
    let mut worker = Worker::start_with(port, vnc::prompt_auth(), false, Timeouts::default(), move |event| {
        if event.kind == 1 { sent.send(()).unwrap(); gate.lock().unwrap().take().unwrap().recv_timeout(WAIT).unwrap(); } true
    });
    ready.recv_timeout(WAIT).unwrap();
    for _ in 0..transport::INPUT_CAPACITY { worker.control.key(0x61, true).unwrap(); }
    assert_eq!(worker.control.key(0x61, false), Err(Failure::InputOverload));
    assert_eq!(worker.control.key(0x61, false), Err(Failure::Closed));
    release.send(()).unwrap(); worker.joined(); assert_eq!(worker.control.poll().phase, 6); assert_eq!(worker.control.poll().error, Failure::InputOverload.to_string()); server.join().unwrap();
}
#[test]
fn graphics_are_worker_only_and_wakes_follow_mutation_and_coalesce() {
    let (port, server) = fixture(move |mut s| { handshake(&mut s); update(&mut s, 1); rect(&mut s, 0, 0, 1, 1, 0); s.write_all(&[1,2,3,0]).unwrap(); eof(&mut s); });
    let owner = Arc::new(Mutex::new(None)); let graphics_owner = owner.clone();
    let graphics_count = Arc::new(std::sync::atomic::AtomicUsize::new(0)); let count = graphics_count.clone();
    let wake_counts = Arc::new(Mutex::new(Vec::new())); let wakes = wake_counts.clone(); let wake_count = graphics_count.clone();
    let (control, backend) = transport::create(BackendTarget { host: "localhost".into(), port, view_only: false }, vnc::prompt_auth(), move |_| {
        let mut owner = graphics_owner.lock().unwrap(); let current = thread::current().id();
        if let Some(owner) = *owner { assert_eq!(owner, current); } else { *owner = Some(current); }
        count.fetch_add(1, std::sync::atomic::Ordering::SeqCst); true
    }, move || { wakes.lock().unwrap().push(wake_count.load(std::sync::atomic::Ordering::SeqCst)); }).unwrap();
    assert!(control.resolved(vec!["127.0.0.1".into()]));
    let thread = thread::spawn(move || backend.run()); wait(|| graphics_count.load(std::sync::atomic::Ordering::SeqCst) >= 2);
    assert_eq!(wake_counts.lock().unwrap().len(), 1, "wake should stay coalesced until poll");
    assert_ne!(owner.lock().unwrap().unwrap(), thread::current().id());
    control.poll(); control.stop(); thread.join().unwrap();
    assert_eq!(control.poll().phase, 5); assert_eq!(wake_counts.lock().unwrap().last(), Some(&3)); // geometry, raw, commit server.join().unwrap();
}
