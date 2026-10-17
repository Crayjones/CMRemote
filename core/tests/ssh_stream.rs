#![allow(non_snake_case)]
use remotemanager_core::ssh::{Failure, stream::{self, StreamCommand}};
use std::{sync::{Arc, atomic::{AtomicUsize, Ordering}}, time::Duration};
use tokio::time::timeout;

fn stream() -> (stream::StreamControl, stream::StreamWorker) { stream::byte_stream(|| {}) }
fn fixture(length: usize) -> Vec<u8> { (0..length).map(|index| (index % 251) as u8).collect() }

#[tokio::test(flavor = "current_thread")]
async fn sshQueuesPreserveBytesAndStayBounded() {
    let (control, mut worker) = stream();
    let expected = fixture(stream::INPUT_BYTES);
    for packet in expected.chunks(stream::PACKET_BYTES) { control.input(packet).unwrap(); }
    assert_eq!(control.stats().input_bytes, stream::INPUT_BYTES);
    assert!(matches!(control.input(b"not accepted"), Err(Failure::InputOverload)));
    assert_eq!(control.stats().input_bytes, stream::INPUT_BYTES);
    let mut received = Vec::new();
    while received.len() < expected.len() {
        let StreamCommand::Input(packet) = worker.next_command().await.unwrap() else { panic!("Expected input") };
        assert!(packet.len() <= stream::PACKET_BYTES); received.extend(packet);
    }
    assert!(received == expected); assert_eq!(control.stats().input_bytes, 0);
    control.input(b"after credit").unwrap();
    let StreamCommand::Input(packet) = worker.next_command().await.unwrap() else { panic!("Expected input") };
    assert!(packet == b"after credit");

    for index in 0..stream::OUTPUT_ENTRIES { worker.output(vec![index as u8]).await.unwrap(); }
    assert_eq!(control.stats().output_entries, stream::OUTPUT_ENTRIES);
    let mut pending = Box::pin(worker.output(vec![200]));
    assert!(timeout(Duration::from_millis(10), pending.as_mut()).await.is_err());
    assert_eq!(control.stats().output_entries, stream::OUTPUT_ENTRIES);
    let drained = control.drain();
    assert!(drained.packets.into_iter().flatten().eq(0..stream::OUTPUT_ENTRIES as u8));
    timeout(Duration::from_secs(1), pending.as_mut()).await.unwrap().unwrap(); drop(pending);
    assert!(control.drain().packets.into_iter().flatten().eq([200]));
    worker.finish(); assert!(control.drain().ended);
}

#[tokio::test(flavor = "current_thread")]
async fn sshStreamOutputBackpressurePreservesOrder() {
    let (control, mut worker) = stream();
    let expected = fixture(stream::OUTPUT_BYTES + stream::PACKET_BYTES);
    for packet in expected[..stream::OUTPUT_BYTES].chunks(stream::PACKET_BYTES) {
        worker.output(packet.to_vec()).await.unwrap();
    }
    let stats = control.stats(); assert_eq!(stats.output_bytes, stream::OUTPUT_BYTES);
    assert!(stats.output_entries <= stream::OUTPUT_ENTRIES);
    let mut pending = Box::pin(worker.output(expected[stream::OUTPUT_BYTES..].to_vec()));
    assert!(timeout(Duration::from_millis(10), pending.as_mut()).await.is_err());
    assert_eq!(control.stats(), stats);
    let drained = control.drain(); assert!(drained.more); assert!(!drained.ended);
    let mut received: Vec<u8> = drained.packets.into_iter().flatten().collect();
    assert_eq!(received.len(), stream::GUI_BYTES);
    timeout(Duration::from_secs(1), pending.as_mut()).await.unwrap().unwrap(); drop(pending);
    worker.finish();
    loop {
        let drained = control.drain();
        assert!(drained.packets.iter().map(Vec::len).sum::<usize>() <= stream::GUI_BYTES);
        received.extend(drained.packets.into_iter().flatten());
        if drained.ended { assert!(!drained.more); break; }
    }
    assert!(received == expected); assert_eq!(control.stats().output_bytes, 0);
    assert!(matches!(control.input(b"closed"), Err(Failure::StreamClosed)));
}

#[tokio::test(flavor = "current_thread")]
async fn sshStopBypassesFullQueues() {
    let (control, mut worker) = stream();
    for packet in fixture(stream::INPUT_BYTES).chunks(stream::PACKET_BYTES) { control.input(packet).unwrap(); }
    for _ in 0..stream::OUTPUT_ENTRIES { worker.output(vec![1]).await.unwrap(); }
    let mut pending = Box::pin(worker.output(vec![2]));
    assert!(timeout(Duration::from_millis(10), pending.as_mut()).await.is_err());
    let observer = control.clone();
    control.stop(); control.stop();
    assert!(matches!(timeout(Duration::from_secs(1), pending.as_mut()).await.unwrap(), Err(Failure::Cancelled)));
    drop(pending);
    timeout(Duration::from_secs(1), observer.cancelled()).await.unwrap();
    assert!(matches!(worker.next_command().await, Err(Failure::Cancelled)));
    assert!(matches!(control.input(b"stale"), Err(Failure::Cancelled)));
    assert!(matches!(control.resize(24, 80), Err(Failure::Cancelled)));
    assert!(control.drain().packets.is_empty());
    drop(worker);
    assert!(control.drain().ended); assert_eq!(control.stats().input_bytes, 0);
    assert_eq!(control.stats().output_bytes, 0); assert_eq!(control.stats().output_entries, 0);

    let (control, mut worker) = stream();
    let mut waiting = Box::pin(worker.next_command());
    assert!(timeout(Duration::from_millis(10), waiting.as_mut()).await.is_err());
    control.stop();
    assert!(matches!(timeout(Duration::from_secs(1), waiting.as_mut()).await.unwrap(), Err(Failure::Cancelled)));
}

#[tokio::test(flavor = "current_thread")]
async fn sshStreamResizeCoalescesAndRejectsInvalidCells() {
    let (control, mut worker) = stream();
    control.input(b"ordered input").unwrap();
    control.resize(24, 80).unwrap(); control.resize(256, 512).unwrap(); control.resize(1, 1).unwrap();
    for (rows, columns) in [(0, 80), (24, 0), (257, 80), (24, 513)] {
        assert!(matches!(control.resize(rows, columns), Err(Failure::TerminalSize)));
    }
    let StreamCommand::Resize(size) = worker.next_command().await.unwrap() else { panic!("Expected resize") };
    assert_eq!((size.rows, size.columns), (1, 1));
    let StreamCommand::Input(packet) = worker.next_command().await.unwrap() else { panic!("Expected input") };
    assert!(packet == b"ordered input");
    assert!(timeout(Duration::from_millis(10), worker.next_command()).await.is_err());
    assert!(matches!(control.input(&[]), Err(Failure::StreamBounds)));
    assert!(matches!(control.input(&vec![1; stream::PACKET_BYTES + 1]), Err(Failure::StreamBounds)));
    assert!(matches!(worker.output(vec![]).await, Err(Failure::StreamBounds)));
    assert!(matches!(worker.output(vec![1; stream::PACKET_BYTES + 1]).await, Err(Failure::StreamBounds)));
    assert_eq!(control.stats().input_bytes, 0); assert_eq!(control.stats().output_bytes, 0);
}

#[tokio::test(flavor = "current_thread")]
async fn sshStreamWakeAndDrainStayBounded() {
    let wakes = Arc::new(AtomicUsize::new(0)); let observed = wakes.clone();
    let (control, mut worker) = stream::byte_stream(move || { observed.fetch_add(1, Ordering::SeqCst); });
    for _ in 0..10 { worker.output(vec![1; stream::PACKET_BYTES]).await.unwrap(); }
    assert_eq!(wakes.load(Ordering::SeqCst), 1);
    let first = control.drain(); assert!(first.more);
    assert_eq!(first.packets.iter().map(Vec::len).sum::<usize>(), stream::GUI_BYTES);
    assert_eq!(wakes.load(Ordering::SeqCst), 2);
    worker.output(vec![2]).await.unwrap(); assert_eq!(wakes.load(Ordering::SeqCst), 2);
    assert!(control.drain().more); assert_eq!(wakes.load(Ordering::SeqCst), 3);
    assert!(!control.drain().more); assert_eq!(wakes.load(Ordering::SeqCst), 3);
    worker.output(vec![3]).await.unwrap(); assert_eq!(wakes.load(Ordering::SeqCst), 4);
    worker.finish(); assert_eq!(wakes.load(Ordering::SeqCst), 4);
    let last = control.drain(); assert!(last.ended); assert!(last.packets.into_iter().flatten().eq([3]));
    worker.finish(); assert_eq!(wakes.load(Ordering::SeqCst), 4);

    let wakes = Arc::new(AtomicUsize::new(0)); let observed = wakes.clone();
    let (control, worker) = stream::byte_stream(move || { observed.fetch_add(1, Ordering::SeqCst); });
    drop(worker); assert_eq!(wakes.load(Ordering::SeqCst), 1);
    assert!(control.drain().ended);
}

#[tokio::test(flavor = "current_thread")]
async fn sshStreamReadAndWriteCanProgressIndependently() {
    let (control, mut worker) = stream();
    let mut commands = worker.take_commands().expect("One input consumer");
    assert!(worker.take_commands().is_none());
    for _ in 0..stream::OUTPUT_ENTRIES { worker.output(vec![1]).await.unwrap(); }
    let mut output = Box::pin(worker.output(vec![2]));
    assert!(timeout(Duration::from_millis(10), output.as_mut()).await.is_err());
    control.input(b"independent input").unwrap();
    let StreamCommand::Input(packet) = timeout(Duration::from_secs(1), commands.next_command()).await.unwrap().unwrap()
        else { panic!("Expected input") };
    assert!(packet == b"independent input");
    let first = control.drain(); assert_eq!(first.packets.len(), stream::OUTPUT_ENTRIES);
    timeout(Duration::from_secs(1), output.as_mut()).await.unwrap().unwrap(); drop(output);
    let mut waiting = Box::pin(commands.next_command());
    assert!(timeout(Duration::from_millis(10), waiting.as_mut()).await.is_err());
    worker.finish();
    assert!(matches!(timeout(Duration::from_secs(1), waiting.as_mut()).await.unwrap(), Err(Failure::StreamClosed)));
    assert!(control.drain().packets.into_iter().flatten().eq([2]));
}

#[tokio::test(flavor = "current_thread")]
async fn sshStreamOwnsOnlyBoundedPacketAllocations() {
    let (control, mut worker) = stream();
    let mut oversized_allocation = Vec::with_capacity(stream::PACKET_BYTES * 4);
    oversized_allocation.extend([1, 2, 3]);
    worker.output(oversized_allocation).await.unwrap();
    let drained = control.drain(); let packet = &drained.packets[0];
    assert_eq!(packet.capacity(), packet.len()); assert!(packet == &[1, 2, 3]);
    worker.finish();
    assert!(matches!(worker.output(vec![4]).await, Err(Failure::StreamClosed)));
    assert!(control.drain().ended);
}

#[test]
fn sshStreamCrossThreadHandoffRejectsRetiredBytes() {
    let (send, receive) = std::sync::mpsc::channel();
    let (control, mut worker) = stream::byte_stream(move || { let _ = send.send(()); });
    let expected = fixture(stream::OUTPUT_BYTES * 2 + 17); let supplied = expected.clone();
    let owner = std::thread::current().id();
    let producer = std::thread::spawn(move || {
        assert_ne!(std::thread::current().id(), owner);
        let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        runtime.block_on(async {
            for packet in supplied.chunks(1937) { worker.output(packet.to_vec()).await.unwrap(); }
        });
        // Normal retirement must retain accepted output until the GUI drains it.
        drop(worker);
    });
    let mut received = Vec::new();
    loop {
        receive.recv_timeout(Duration::from_secs(2)).expect("Expected queued wake");
        let drained = control.drain();
        assert!(drained.packets.iter().map(Vec::len).sum::<usize>() <= stream::GUI_BYTES);
        let stats = control.stats(); assert!(stats.output_bytes <= stream::OUTPUT_BYTES);
        assert!(stats.output_entries <= stream::OUTPUT_ENTRIES);
        received.extend(drained.packets.into_iter().flatten());
        if drained.ended { break; }
    }
    producer.join().unwrap(); assert!(received == expected);
    assert!(control.drain().packets.is_empty());

    let (control, mut worker) = stream();
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
    runtime.block_on(worker.output(vec![1, 2, 3])).unwrap();
    control.stop(); drop(worker);
    assert!(control.drain().packets.is_empty());
}
