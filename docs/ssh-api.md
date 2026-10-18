# Embedded SSH2 and terminal appearance

Saved SSH2 profiles and SSH Quick Connect use the production MainWindow, a russh transport and a GUI-owned libvterm terminal. No external PuTTY/ssh process or window reparenting is involved. Native Cocoa and Linux ARM64 XCB acceptance uses owned OpenSSH/vim fixtures; [validation](validation.md) separates that evidence from component tests and package smoke.

## Metadata and authentication

Core owns nonsecret SSH target/profile metadata: configured host/port, literal Username, authentication method and key-file path. Domain is preserved but inactive. Native SSH2 aliases, inheritance and unknown XML content round-trip; SSH1 remains unsupported. Protocol-scoped version-2 bindings authorize SSH Password reuse through the owning file's review/password workflow. Exact version-1 RDP bindings still read. Private-key mode never uses a saved Password as a passphrase; changing away from Password removes reusable-password authorization without deleting its ciphertext.

Saved Connect/Reconnect captures the runtime DocumentId, generation/revision and committed target. Copied portable UUIDs do not share sessions or credentials. Quick Connect selects RDP or SSH, defaults to RDP, and registers a temporary session only after credential acceptance. SSH defaults to port 22. Save as connection stores metadata, not session authentication, and does not retarget the running tab.

`app/SshAuthentication.h` transfers a move-only, session-only `Secret` through the separate `rm::ssh` cxx bridge:

```cpp
rust::Box<rm::ssh::Authentication> sshPasswordAuthentication(Secret password);
rust::Box<rm::ssh::Authentication> sshPrivateKeyAuthentication(
    const QString& path, Secret passphrase);
```

The opaque Rust object is not a Core method, profile field or session record. The adapter performs no key I/O. Document masters and document crypto keys stay C++-owned. The masked `SshCredentialsDialog` clears its editor/undo history on completion and has no Remember, Domain or RDP clipboard-sharing control. Cancel prevents registration; changed/locked owners invalidate pending attempts. Authentication buffers are released after authentication/failure/cancellation, without a guaranteed-zeroization claim.

The worker bounds key files to 1 MiB and passwords/passphrases to 4096 UTF-8 bytes. It checks regular-file descriptors, rejects final-component symlinks/special files and checks cancellation between bounded reads. OpenSSH decoding checks algorithm/KDF limits before decryption: Ed25519, ECDSA P-256/P-384/P-521, RSA 2048–8192 significant bits, bcrypt rounds 1–64 and nonempty salt up to 64 bytes. Decoder tests cover this format/limit set; successful encrypted-key server acceptance uses Ed25519, not every key type/size. PEM, PPK, certificates, FIDO and agent support are not promised. An individual filesystem or cryptographic operation is not guaranteed interruptible.

## Host trust

`makeSshHostKey` calls the strict Rust public-key identity constructor. It canonicalizes the configured hostname/IP, retains the configured port, validates the exact SSH public-key wire bytes and recomputes the SHA256 fingerprint. Trust uses configured host/port/algorithm, not a resolved address or connection-file path.

`SshHostKeyStore` is blocking. Production uses `SshHostKeyService`, which serializes it on its own Qt worker with request IDs, immutable generations and cancellation. Store tickets bind an inspected cache revision/existence and candidate; inspection alone never authorizes authentication.

The per-user location is `applicationSshHostKeyPath(applicationDataDirectory(...))`, separate from XML/Open/Save as/rekey. `app/main.cpp` supplies it independently of the connection path. Development mode contains it under `.dev/<platform>/data`; tests inject temporary paths. The application never imports or modifies `~/.ssh/known_hosts`.

Schema 1 JSON is limited to 1 MiB/1024 records. Records contain canonical `host`, `port`, `algorithm`, Base64 `key` and checked `fingerprint`. Duplicate/unknown fields, duplicate endpoint/algorithm records, malformed/noncanonical keys, unsafe paths and unexpected schema versions fail closed.

Unknown keys offer **Trust and save / Connect once / Cancel**. Changed keys show old/new fingerprints and offer **Replace trusted key and connect / Connect once without replacement / Cancel**. Dialogs use plain text and default/escape Cancel. Host approval precedes authentication. Connect once never writes. Durable choices must complete a conflict-checked `AtomicFile` replacement; write failure has no automatic fallback. A committed sync warning requires acknowledgment before authorization, and Cancel does not undo that committed entry. Stale/conflicting requests cannot authorize and require fresh inspection.

## Transport and ownership

`SshWorker` is a GUI-affine facade over a standalone QThread/current-thread Tokio backend. No worker borrows Core, a QWidget or libvterm state. Qt performs asynchronous hostname lookup; abort invalidates delivery, without claiming that the OS resolver always stops immediately. Rust receives owned resolved addresses while retaining the configured hostname for trust.

Connect/handshake/authentication/channel operations have cancellation-aware stage deadlines: 30 seconds ordinarily, 120 seconds for host approval, with a five-second connection attempt per resolved address. Connected is published only after authentication and successful PTY (`xterm-256color`) and shell replies. Refused requests, EOF and failures end the session. Unsolicited forwarding/X11/agent channels are denied; there is no exec, tunnel or startup-command interface. Public failures are fixed messages, not raw library/server diagnostics.

`core/src/ssh_stream.rs` owns ordered input/output queues, independent of Core and authentication. Commands and byte payloads have no Debug/serialization implementation; numeric stats contain counts only.

| Resource | Bound |
|---|---|
| Input byte ring | 256 KiB |
| Output mailbox | 1 MiB and 64 packets |
| Input/output packet | 1–32 KiB |
| GUI drain | 128 KiB |
| Latest coalesced resize | 1–256 rows, 1–512 columns |
| SSH receive window | 256 KiB |

Input accepts a complete bounded chunk or returns a fixed error without changing the queue. Output waits for credit while retaining its packet; accepted bytes are not dropped/reordered. One bounded producer packet and the current bounded GUI drain can exist outside the mailbox. A coalesced queued wake delivers bytes through the GUI facade directly to the current terminal parser.

Stop uses a separate cancellation signal, independent of data capacity. Normal finish retains accepted output until the final drain; cancellation discards retired output. Generation/page/request guards reject stale delivery. Workers retire outside SessionPane, join off the GUI and delete only after exit. Reconnect waits asynchronously for retirement. Tab/panel/application close invalidates callbacks and stops owned workers before final destruction. Disconnected tabs keep their registrations and file-close/delete guards until closed. RDP retains its separate FreeRDP teardown behavior.

## Terminal and clipboard

`TerminalScreen` owns libvterm on the GUI thread. `TerminalView` renders UTF-8/wide/combining cells, attributes, cursor and alternate screen; it encodes keyboard/IME and bracketed paste through libvterm. Logical font metrics drive bounded rows/columns and PTY resize, not framebuffer pixels. Scrollback is limited to 10000 lines/16 MiB; paste is limited to 1 MiB UTF-8 and drains incrementally under backpressure.

Selection and **Copy/Paste** require direct user actions through the injected `ClipboardEndpoint`. Disconnected text can still be copied; inactive input/paste is disabled. There is no automatic terminal clipboard synchronization. OSC selection callbacks are unregistered; remote titles, OSC/DCS and hyperlinks cannot access the host clipboard or open/execute local resources. Tests use in-memory endpoints and never the real host clipboard. RDP's renderer and opt-in CLIPRDR behavior remain separate.

## Indexed and truecolor output

SGR `38;5;n` and `48;5;n` retain indexed foreground/background values in libvterm cells. `TerminalScreen::convert` resolves them for live, retained-history and alternate-screen cells; `TerminalView` paints the resulting QColor directly:

- **0–15:** configured ANSI palette, with the existing optional bold-to-bright foreground rule for 0–7.
- **16–231:** standard xterm 6×6×6 cube, using channel levels `0,95,135,175,215,255`.
- **232–255:** grayscale intensity `8 + 10 × (index − 232)`, from 8 to 238.
- **Explicit RGB:** SGR `38;2;r;g;b` and `48;2;r;g;b` keep the supplied channels without quantization.

Index 28 is `(0,135,0)`, `#008700`, matching equivalent truecolor in either channel. SGR 39/49 restores configured foreground/background defaults; SGR 0 resets colors and attributes. Theme changes affect defaults and indices 0–15, not extended indices or direct RGB. OSC 4 dynamic palette overrides are not implemented.

The pinned libvterm 0.3.3 lookup instead uses cube levels `0,51,102,153,204,255` and a grayscale ramp spanning 0–255. Delegating extended resolution to it caused index 28 to become `#006600` while direct RGB remained `#008700`. The local resolver corrects all extended indices without changing the parser, raw-cell storage, renderer, dependency, SSH PTY (`xterm-256color`), terminfo or tmux configuration. Owned shell/tmux acceptance checks final received colors; it does not claim tmux preserves the original SGR encoding. See [T52 verification](validation.md#standard-xterm-indexed-colors--t52-2026-10-11).

## Scrollback selection

Left-button dragging selects a continuous character range in either direction. The original starting cell stays fixed as wheel/trackpad input or scrollbar changes move the viewport during a drag. Releasing finishes the selection; subsequent scrolling does not extend it. Copy remains explicit and preserves wide/combining characters, trims row-end spaces and inserts newlines between selected rows. Dragging and scrolling send no SSH input and perform no clipboard reads, writes or watches.

A parent-owned 50 ms timer scrolls 1–8 rows per tick while a held pointer is at or beyond the top/bottom text-row edge. Distance outside the grid controls the bounded rate; the scrollbar and horizontal positions outside the text grid do not trigger it. Returning to the interior or reaching a history boundary stops the timer. Release, lost left-button motion, mouse ungrab, focus/window deactivation, hide, resize and destruction stop dragging. Focus loss can leave a valid completed selection available for later Copy.

Wheel input prefers Qt's pixel delta when available, otherwise its angular delta. Fractional pixels accumulate to a cell row; angular movement uses three rows per 120-unit notch. The two delta types are never added together, horizontal-only movement is ignored, and Qt's delivered direction is used without a second inversion. Gesture/geometry boundaries and blocked history boundaries reset stale remainder.

Endpoints use absolute retained-line positions plus columns. History-only selection and the reading viewport stay anchored as output appends or older unselected lines are evicted; endpoint eviction clears selection instead of substituting newer text. Normal output follows the bottom only when already there and not dragging. Cursor-only and color-only changes preserve selection. Any live-grid content mutation conservatively clears a selection touching the live grid, even if the changed cell lies outside the selected range. Resize, history clear and primary/alternate transitions also clear it, including transitions out and back within one parser feed. No whole-history selection snapshot or per-output scan is required.

Alternate-screen programs allow selection within visible cells only, with no primary-history scrolling or edge timer. This reads retained local terminal text, not remote tmux history. Reflow, word/rectangle selection and double/triple-click selection are not implemented. Existing history/drain limits and remote mouse-reporting behavior are unchanged.

## Keyboard routing

Focused terminal input claims Qt shortcut overrides and handles Tab/back-tab before widget focus traversal. Physical Ctrl+A–Z emits conventional C0 bytes independent of Qt's letter/control/empty event text; Ctrl+I/J/M are Tab/LF/CR, not CSI-u. Ctrl+Space/@ and the standard 2–8 punctuation aliases cover NUL, ESC, FS, GS, RS, US and DEL. Alt/Option ASCII editing keys are normalized to one ESC prefix; ordinary Unicode/IME commits stay unchanged. Special keys retain libvterm modifier/application-cursor encoding. Qt maps physical Control to MetaModifier and Command to ControlModifier on macOS; unrelated Command application shortcuts remain available.

Remote programs interpret these bytes. The examples below describe Bash/readline's conventional emacs bindings, not local shell emulation or a guarantee for every remote configuration.

| Remote keys | Conventional action |
|---|---|
| Ctrl+A/E, Ctrl+B/F | Line start/end, character backward/forward |
| Ctrl+P/N, Ctrl+R/G | History previous/next, reverse search/cancel search |
| Ctrl+U/K/W/Y, Ctrl+H/T | Kill before/after cursor, delete word, yank, backspace, transpose |
| Ctrl+C/D/Z/L | Interrupt, EOF/delete, suspend, clear/redraw |
| Ctrl+S/Q | Pause/resume when remote flow control is enabled |
| Ctrl+X then following keys | Remote readline/application prefix bindings |
| Alt+B/F/D, Alt+Backspace | Word backward/forward/delete, delete previous word |
| Alt+T/U/L/C, Alt+Enter | Word transpose/case operations, Meta+Enter |
| Escape then a key | Remote Meta/prefix input |
| Tab / Shift+Tab | Completion / back-tab; no widget focus traversal |
| Arrows, Home/End, Insert/Delete, PageUp/Down, F1–F12 | Remote special keys with supported Shift/Alt/Ctrl modifiers |
| tmux Ctrl+B then C/N/P/W/%/\"/[/D | Create/navigate/list windows, split, copy mode, detach according to remote tmux bindings |

Ctrl+L sends `0x0c`; Ctrl+B then ordinary C sends `0x02c`. There is no local clear/tmux state machine or application-window creation. Remote clear retains local scrollback. The app does not install tmux on user servers.

| Local action | Linux / non-macOS | macOS |
|---|---|---|
| Copy / paste | Ctrl+Shift+C/V; Ctrl+Insert / Shift+Insert aliases | Command+C/V; Shift+Insert paste where available |
| Select all retained cells/history | Ctrl+Shift+A | Command+A |
| Primary scrollback page up/down | Shift+PageUp / Shift+PageDown | Shift+PageUp / Shift+PageDown |
| Primary scrollback start/end | Ctrl+Shift+Home / Ctrl+Shift+End | Command+Home / Command+End |
| Session font +/− | Ctrl+Plus/Equals / Ctrl+Minus | Command+Plus/Equals / Command+Minus |
| Reset font to global size | Ctrl+0 | Command+0 |

Selection/scrolling do not access the clipboard or send input. Copy remains available while disconnected; paste still requires active input and follows the existing bounds/backpressure. Plain page keys remain remote. In alternate-screen programs, modified scrollback keys also go remote; macOS Command+Home/End uses Ctrl+Shift+Home/End encoding. Clipboard and zoom shortcuts remain local in either buffer. Context actions list local keys without accessing the clipboard.

## Terminal Appearance

The **Terminal** page of the Preferences-role Settings dialog edits it; **Terminal Settings…** in SSH terminal/tab context menus opens that page. Its real TerminalView preview contains only hardcoded synthetic text and uses a no-access clipboard endpoint; it never mirrors a connected terminal.

Preferences are app-wide: fixed-width font family, size 6–48 pt, default text/background, selection/cursor colors, all 16 ANSI colors, cursor shape/blink and bold-color behavior. **Classic (PuTTY-inspired)** uses intensity 187 for normal primaries instead of libvterm's 224. **Tomorrow Night**, **libvterm original** and custom edits are available. Presets change colors only and are recognized by exact color equality, without a persisted preset-name field. This is a declared palette, not pixel-identical PuTTY behavior or PuTTY settings import.

**Apply** persists before publishing to open SSH tabs; **OK** persists and closes. **Cancel** discards changes since the last successful Apply. **Reset Defaults** changes only the draft/preview until applied. Opening, unchanged Apply and unapplied Reset/Cancel do not replace the preference file. Save failure retains the draft and prior applied bytes; committed durability warnings are acknowledged as saved.

`TerminalAppearanceService` performs bounded schema-1 JSON load/save off the GUI through `AtomicFile`. `terminal-appearance.json` is limited to 64 KiB and stored separately from XML, Core targets, credentials and trust. Missing files use defaults without a startup write. Invalid files report an error and cannot be silently overwritten; explicit Reset/Apply can replace an inspected malformed regular file, never an unsafe path.

Palette application updates tagged defaults/ANSI indices 0–15 in live/history/alternate buffers without refeeding or clearing them. Explicit RGB and indices 16–255 are unchanged. Font changes recompute logical cells and reach the existing PTY resize path. Existing, disconnected, new and reconnected SSH tabs receive current preferences without an Apply-induced reconnect or target/identity change; RDP rendering is unaffected.

Tomorrow Night uses foreground/selection background/cursor `#eaeaea` and background/selection foreground `#1d1f21`:

| ANSI | Normal | Bright |
|---|---|---|
| Black | #1d1f21 | #666666 |
| Red | #cc6666 | #d54e53 |
| Green | #b4bd68 | #b9ca4b |
| Yellow | #f0c674 | #e6c547 |
| Blue | #81a2be | #7aa6da |
| Magenta | #b294bb | #c397d8 |
| Cyan | #8abeb7 | #70c0b1 |
| White | #c6c8c6 | #eaeaea |

Font zoom is a bounded, in-memory per-view offset from the app-wide base, in 1 pt steps. Effective size stays within 6–48 pt; repeated keys at a bound do not accumulate hidden overshoot. Global Apply updates family/palette/cursor/bold and retains the clamped offset. A disconnected view or canceled credential reconnect retains its size; successful reconnect/new view starts at the current global base. Ctrl/Command+0 resets the offset without saving. Zoom emits no SSH input, changes no target/session identity and writes no XML/preferences/credentials/trust data; it reaches PTY resize through existing font metrics/cellsChanged wiring. Other tabs, RDP, editors and the synthetic preview are not zoom targets.

## Platform evidence and limits

On 2026-10-11, macOS ARM64 Cocoa and Linux ARM64 XCB passed all ten independent OpenSSH acceptance cases, including saved passwords, encrypted Ed25519 keys, temporary sessions, explicit trust choices, Unicode vim editing, shell editing/completion/interrupt/redraw, tmux window/split/detach commands, server-reported window/font/session-zoom PTY resize, exact drag selection through retained shell output, indexed/truecolor foreground/background equivalence outside and inside tmux, and cancellation/teardown. Complete full/mock-only regressions passed after the indexed-color correction. Earlier relocated development packages also passed; they were not rebuilt for this follow-up. Package probes exercise startup/linkage/notices and RDP rendering/crypto, not remote SSH connections from the archives.

Windows, AMD64 and Wayland SSH are untested. Earlier AMD64 RDP/package evidence does not establish SSH support. The project is GPL-2.0-or-later; original-icon provenance notes are in [dependencies](dependencies.md). See [dependencies](dependencies.md) for exact pins/notices and [validation](validation.md) for commands, receipts and native-smoke skips.
