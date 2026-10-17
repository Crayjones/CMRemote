# Local integration patches to vnc-rs 0.6.0

The published archive, revision, checksum and license evidence are in
`SOURCE-PROVENANCE`. The byte-for-byte baseline is `../vnc-rs-0.6.0.crate`.
`patches/vnc-rs-0.6.0.patch` is the unified source diff against that archive.
Neither published manifest nor license/embedded notice has been rewritten.

## Patch inventory

1. `src/client/connector.rs`: add exact `AuthenticationPolicy` selection.
   Default is `RequireVncAuth`, including RFB 3.3 single-type negotiation;
   `ExplicitNone` selects None only when offered and never substitutes VncAuth.
   `NoneIfOffered` (used by the application, matching mRemoteNG) selects None
   whenever offered, so no credential is sent, and otherwise VncAuth. The
   credential callback is only polled once VncAuth is selected, which lets the
   application ask for a password only when the server requires one. A failed
   VncAuth result is reported as `WrongPassword` for every RFB version; the RFB
   3.8 reason string is read (bounded) and discarded, never surfaced.
2. `src/client/connection.rs`: reduce the network queue from 4096 to 16
   packets. At 65535 bytes/packet it retains at most 1,048,560 queued payload
   bytes, plus one network read buffer and the decoder's currently held input.
   Keep existing asynchronous backpressure and independent stop channels.
   Reduce the internal input queue from 4096 to 16 as a companion safety bound.
   The application submits only fixed-size key, pointer and refresh messages;
   clipboard input and client-requested resizing are not exposed.
3. `src/event.rs` and `src/client/connection.rs`: emit `UpdateComplete` after
   the entire framebuffer update, including a zero-rectangle update. The
   application counts the library's initial full request, then sends one
   incremental request after completion: immediately after an update with
   pixel changes, otherwise paced at a minimum 16 ms. A static
   desktop with a pending incremental request has no idle-session timeout.
4. `src/client/connector.rs` and `src/client/auth.rs`: best-effort wipe the
   upstream-required transient UTF-8 password copy and the authentication
   helper's DES key/challenge on normal drop or cancellation. The application
   uses opaque `credentials::Secret`, validates 1–8 printable ASCII bytes,
   and creates a String only when the library actually requests a password.
   This is not locked memory or a guaranteed zeroization claim; DES internals,
   compiler temporaries, socket buffers and native dialog buffers may retain
   copies. Embedded DES notice/source is unchanged.
5. `src/client/connection.rs` and `src/client/connection/output.rs`: remove
   server-name, clipboard-message and raw decoder-error log payloads. Application
   errors are fixed generic categories, never server-supplied strings. Published
   examples remain upstream examples and are not run by this integration.

## Application adapter safety boundaries (outside the vendored patch)

`core/src/vnc_transport.rs` runs one current-thread Tokio runtime on the native
worker and drops it before final terminal metadata. It only uses bounded
nonblocking `poll_event` batches (16) with a yield, never the mutex-holding
`recv_event` API. Productive turns recheck output promptly after servicing
cancellation, due refresh and ordered input; only empty turns use the 4 ms idle
poll fallback. This avoids per-tile sleeps when the two-event decoder queue
refills during the cooperative yield, without expanding queues or dropping
deltas. Graphics callbacks are synchronous, worker-only, followed by
coalesced wake; a rejected mutation fails closed. The callback ABI uses BGRA
rectangles, stable-source CopyRect metadata, geometry and mask-alpha cursors.

Native asynchronous DNS supplies at most 16 validated IP addresses. Resolution,
connection, handshake, socket writes and queue waits are cancellable/bounded;
independent watch cancellation cannot be blocked by input/output queues. Socket
write stalls are bounded without imposing a connected read deadline. The
application input queue is 128 entries. A full queue fails closed with a visible
error rather than silently dropping transitions; view-only is checked in Rust.

Only ZRLE, CopyRect, Raw, CursorPseudo and DesktopSize are advertised. Tight/JPEG,
TRLE, ExtendedDesktopSize, clipboard and client resizing are not enabled. The
backend validates every delivered event, including cursor dimensions (256/axis),
hotspots, payload length and hidden-cursor normalization, before native mutation.

## Enabled-decoder audit and memory limits

The published engine already checks desktop sides <=8192 and pixels <=8,294,400.
Raw allocations are <=33,177,600 bytes. CopyRect checks source and destination
using widened coordinates; DesktopSize validates geometry and zero origin.
ZRLE compressed input is <=64 MiB and inflates through bounded 64x64 tiles, not
an unbounded output buffer. Palette indices and runs are checked; malformed
streams return errors and do not retry zero-byte inflation forever. Server
names/authentication reasons are <=4096 wire bytes; clipboard <=1 MiB wire bytes
and is ignored by this backend. Lossy UTF-8 expansion remains bounded.

The output queue retains at most two decoded events. Queued output, compressed
input, framebuffer copies, cursor temporary buffers, the packet queue, socket
buffers and native presentation memory are additive. Infallible allocations can
still abort under process-wide memory pressure. These limits are not a
whole-process memory proof. The audit found no required enabled-decoder rewrite;
invalid upstream cursor metadata is rejected by the application boundary.

## Regression coverage

`core/tests/vnc_transport.rs` uses owned loopback/duplex RFB fixtures only:
3.3/3.7/3.8 classic authentication and independent DES vectors, mixed/unknown
security offers, default None denial and explicit None, generic failure text,
fragmented negotiation, bounded malformed graphics, Raw/ZRLE/CopyRect/cursor/
resize, sink rejection, view-only, initial/empty-update pacing, idle input,
worker-only callbacks, wake coalescing, deadlines, independent cancellation,
saturated decoding and transition-preserving input overload. A 1920×1080 ZRLE
regression validates all 510 tiles, exact pixels/order/extents and completion
through the next incremental request against a generous 500 ms scheduling bound.
Explicit gates prove ordered key/button transitions arrive before tile completion
and cancellation interrupts an unfinished frame. These are owned-loopback debug
backend measurements, not native scanout/FPS or remote-network benchmarks.

The root workspace excludes this package so its original minifb example/test
GUI dependencies are not application's workspace defaults. Cargo.lock pins the
actual application dependency closure; parent dependency audits handle notices
and all-target licensing. No release clearance or verified Windows native
presentation is implied by these source-level boundaries and loopback tests.
