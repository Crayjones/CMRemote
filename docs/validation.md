# Production Config, Connections and protection validation

Dated runs below cover Apple Silicon/macOS 27.0 with existing Apple tools, and Linux ARM64 in the pinned Ubuntu 24.04 development image through OrbStack. Native macOS and Linux builds are separate. The owned RDP server uses a separately pinned Ubuntu 26.04 fixture; the owned OpenSSH/vim fixture uses the pinned development base. `Cargo.lock` remains tracked. Older sections retain their original failures and evidence limits; the current T48–T51 results do not complete unrelated renderer/performance tasks.

## Config reference help, defaults and connection icons — T62–T64, 2026-10-12

Config help shows one short description per property: the original English mRemoteNG text where one exists, otherwise a single sentence in the same style. Tooltips match. Dock headers are blue with white captions and white float/close buttons. The four exclusive modes are Properties (always), Inheritance (items inside a folder) and Default Properties/Default Inheritance (connections file root only); Inherit All affects only visible editable fields and preserves hidden protocol overrides.

Creation defaults are bounded, validated nonsecret application preferences saved atomically off the GUI thread. Older version-1 preferences remain readable. Options/defaults saves retain one another's settings; failed writes retain the prior applied template and support Retry/Revert. Existing profiles and running sessions are unchanged. New connections start with an explicit empty hostname, including beneath imported folders with a hidden endpoint. Default Password is only an inheritance policy: creation and that flag share one document replacement, with no saved-password reuse authorization granted automatically. Passwords and key passphrases cannot be default values.

Both icon pickers use the shared 39-name bundled catalog. Typed Icon/InheritIcon metadata round-trips through Core and native XML. Tree/menu/session-tab icons agree without reconnecting or changing captured session targets. Unknown imported names remain metadata with a safe fallback, never filesystem lookup. Duplicate profile IDs remain scoped to their owning document.

### Final verification

| Command | Result | Receipt |
|---|---|---|
| `QT_QPA_PLATFORM=offscreen python3 scripts/project.py test` | 82 Rust tests, 103 Python tests, 29/29 CTest executables passed | `build/macos/config-regressions.log` |
| `python3 scripts/test-cocoa.py property_grid_test` | 45 Qt checks passed; zero failures/skips | `build/macos/config-cocoa-final.log` |
| `python3 scripts/docker.py test --arch amd64` | 82 Rust tests, 103 Python tests, 29/29 CTest executables passed | `build/linux/amd64/config-regressions.log` |
| `python3 scripts/docker.py shell --arch amd64 -c 'python3 scripts/test-native.py --platform xcb property_grid_test'` | 45 Qt checks passed; zero failures/skips | `build/linux/amd64/config-xcb.log` |

Named regressions cover exact reference help/inheritance wording; dock close/float/reset; all four modes; locked-file defaults; restart and creation-only application; one-replacement password inheritance; malformed/secret preference rejection; old preferences/no-op saves; save failure Retry/Revert; both icon pickers; unknown names; retained viewport; duplicate-file ownership; native XML preservation; and UTF-8 icon bounds.

### Corrected failures and native receipts

- A 170-pixel label column clipped value cells in the 226-pixel Config dock. `configValueCellsStayVisibleInNarrowDock` failed first on the Name cell (`build/macos/config-narrow-red.log`). The 140-pixel initial column passes visibility checks in all four modes at 226/330 pixels and normal mouse editing; the column remains manually resizable.
- New connections previously copied a preserved hidden hostname from an imported parent folder. `newConnectionDefaultsLeaveHostnameEmpty` failed first (`build/macos/config-endpoint-red.log`). Explicit empty-host creation now passes while retaining username/other setting inheritance.
- One post-fix Cocoa run reported an early close in the existing `popupSelectionAndEscape` test: 44 passed, one failed (`build/macos/config-cocoa.log`). The isolated case then passed, and the unchanged complete suite passed 45/45. No assertion was removed or weakened; the intermittent popup cause is not established.

Final Cocoa report: `build/macos/native-tests/property_grid_test-4086d03731084703ad13924db9b380fe/results.txt`. Synthetic nonsecret widget receipts are under `build/macos/config-evidence/cocoa/`: `properties.png`, `default-properties.png`, `default-inheritance.png`, `icon-menu.png`, `icon-dropdown.png`. No user connection data, secret dialog/editor, host desktop or clipboard was captured.

Linux AMD64 ran in the owned pinned development container under emulation on the Apple Silicon host, with native XCB checks under owned Xvfb; this is not physical AMD64 hardware testing. XCB receipts are under `build/linux/amd64/config-evidence/xcb/` with the same five filenames. Native receipts on both platforms were inspected for white headers, reference help, four-mode presentation and populated icon pickers.

Windows/Wayland and the original .NET runtime were not tested for this Config work. Reference wording and XML compatibility evidence come from local source/resources and tests, not Windows runtime proof. Original icon redistribution rights remain unverified. No package, commit, push or transfer was performed.

## VNC productive tile-drain scheduling — 2026-10-12

### Reproduction and adapter-only fix

The VNC client uses TCP with TCP_NODELAY, not UDP. The adapter previously slept 4 ms after every bounded decoder poll, even after consuming events. The decoder's two-event output queue refills during the current-thread Tokio cooperative yield, so a 510-tile full-HD ZRLE update repeatedly incurred artificial waits. `core/src/vnc_transport.rs` now immediately rechecks after productive turns, with cancellation, due refresh and ordered input taking priority. Empty turns retain the 4 ms idle fallback. The 16-event budget/yield, queue capacities, one outstanding request, 16 ms refresh pacing, asynchronous retirement and shared progressive QRhi presentation are unchanged. No engine-source, dependency or lockfile changes belong to this repair.

The fail-first owned-loopback regression uses one actual 1920×1080 ZRLE rectangle, independently generated Python zlib bytes with Z_SYNC_FLUSH, and validates all 510 tiles: order, coordinates, final 56-pixel row, colors, opacity and every pixel. Timing ends when the server receives the next incremental request, proving UpdateComplete was consumed. A generous **500 ms** scheduling ceiling was selected before the first run and remains unchanged after the fix; it is not a rendering-speed guarantee.

- Before: **1.783342875 s**, pixel checks passed, elapsed assertion failed (exit 101). Receipt: `build/macos/vnc-scheduling-before.log`.
- Initial fixed run: **117.742292 ms**, passed. Receipt: `build/macos/vnc-scheduling-after.log`.
- Full VNC transport suite: **17/17 passed**; tile workload **113.566875 ms**. Explicit gates also verify exact ordered key/button press/release delivery before frame completion and cancellation of an unfinished tile burst under 500 ms. Receipt: `build/macos/vnc-transport-fixed.log`.
- Ten isolated repeats: **10/10 passed**, **91.779667–92.470666 ms**. Receipt: `build/macos/vnc-scheduling-repeated.log`.
- Full `python3 scripts/env.py cargo test --locked`: **78 Rust tests passed**, exit zero. Receipt: `build/macos/vnc-scheduling-rust.log`.

### Production-shell and regression gates

Both project test commands rebuilt their targets, passed **78 Rust tests** and **103 Python tests**, and exited zero. Full builds passed **29/29 CTest executables**; RDP-disabled/mock-only builds passed **26/26**. These suites retain their offscreen/native exclusions and do not replace independent native evidence.

| Command | Receipt |
|---|---|
| `python3 scripts/project.py test` | `build/macos/vnc-scheduling-project.log` |
| `python3 scripts/project.py test --mock-only` | `build/macos/vnc-scheduling-mock.log` |

`python3 scripts/test-vnc.py` and `python3 scripts/test-vnc.py --platform cocoa` each passed **all six owned production-shell cases**, with zero failures/skips (18 QtTest checks including setup/cleanup per platform). Saved rendering, temporary input, fullscreen geometry, disconnect/reconnect and responsive close retain their checks. The added `vncZrleTileBurstThroughShell` uses two full-HD updates sharing persistent zlib history, verifies every CPU framebuffer pixel and next-request latency, and requires independent QRhi readback plus owned-window composition on native platforms. On macOS the window-composition capture needs Screen Recording access for `vnc_integration`. The test checks access without prompting and fails with a clear message when it is missing; grant it in System Settings → Privacy & Security → Screen Recording before running `--platform cocoa`.

- Offscreen tile updates: **100.160 ms / 101.228 ms**. Receipt: `build/macos/vnc-scheduling-offscreen.log`. `RM_VNC_NATIVE_COMPOSITION=0`; no GPU/composition claim.
- Native Cocoa tile updates: **103.873 ms / 100.701 ms**. Receipt: `build/macos/vnc-scheduling-cocoa.log`. Every case reported `RM_VNC_NATIVE_COMPOSITION=1`. Both tile updates passed QRhi readback and `QScreen::grabWindow(owned-window)` composition sampling after exact CPU framebuffer verification; captured images are in `build/macos/test-data/vnc-vncZrleTileBurstThroughShell-shrio2yx/evidence/`. Capture/readback is a separate correctness check, not included in these transport-to-next-request latency values.

The final focused read-only review found no transport/test-gating defect, but identified DesktopSize ordering in the new tile fixture: RFC 6143 requires it to be the last rectangle. That case now sends a resize-only update, followed by the ZRLE rectangle after the next request, retaining persistent compression history. The earlier receipts above are initial results, not the final fixture revision.

After that fixture-only correction, both acceptance modes again passed **6/6 cases**, zero failures/skips, and all **103 Python tests** passed:

| Final command | Receipt | Tile updates through next request |
|---|---|---|
| `python3 scripts/test-vnc.py` | `build/macos/vnc-scheduling-fixture-final-offscreen.log` | 101.100 / 104.199 ms |
| `python3 scripts/test-vnc.py --platform cocoa` | `build/macos/vnc-scheduling-fixture-final-cocoa.log` | 104.573 / 100.526 ms |
| `python3 scripts/env.py python3 -m unittest discover -s tests -p 'test_*.py' -v` | `build/macos/vnc-scheduling-fixture-final-python.log` | Not a latency test |

The final Cocoa case again verified both exact CPU frames, QRhi readback and independently composed owned-window pixels; captures are in `build/macos/test-data/vnc-vncZrleTileBurstThroughShell-fsoe4olt/evidence/`. No production source changed after the full/mock regression gates. No application test failures occurred after the scheduling fix in these runs. The deliberate pre-fix latency failure remains above; no timing bound was loosened to hide it.

### Evidence limits

These timings are debug-build owned-loopback scheduling measurements including intentional refresh pacing, not native scanout/FPS or a benchmark of the user's server/network. Progressive tile presentation and RTT-sensitive single-request policy remain unchanged. No user server/desktop/accounts or host clipboard are accessed. The existing native cases and this new tile case do not establish Linux/Windows parity, independent password-server interoperability or release/dependency clearance. The pre-existing SPEC description of VNC as deferred is outside this focused scheduling repair.

## Standard xterm indexed colors — T52, 2026-10-11

SGR indexed colors 16–255 now resolve through the standard xterm palette instead of libvterm 0.3.3's non-xterm ramps. Indices 0–15 still honor configured themes and bold rules; explicit 24-bit RGB passes through unchanged.

### Root cause and fix

The pinned libvterm lookup (`pen.c` ramp6/ramp24) uses cube levels 0/51/102/153/204/255 and a grayscale ramp spanning 0–255. SGR parsing stored indices correctly, but `TerminalScreen::convert` delegated resolution to that lookup, so index 28 rendered `#006600` while equivalent truecolor `(0,135,0)` rendered `#008700`. `TerminalView` paints the resolved QColor directly; no interpolation or second remapping was involved. The fix resolves indices 16–231 through xterm levels 0/95/135/175/215/255 and 232–255 through intensity `8 + 10 × (index − 232)` in the existing converter, covering live, retained-history and alternate-screen cells. No parser, renderer, dependency, SSH transport, terminfo or tmux configuration changed. OSC 4 dynamic overrides remain unimplemented.

### Native component tests

Fail-first runs reproduced the mismatch before the fix: index 28 actual `#006600` vs `#008700`, grayscale 232 `#000000` vs `#080808`, 255 `#ffffff` vs `#eeeeee`. Receipts: `build/macos/t52-build-fail-first.log`, `build/macos/t52-colors-fail-first.log`.

Cocoa and Linux ARM64 XCB each passed **320 appearance checks** and **121 terminal checks**, zero failures/skips. The five new named tests check all 16 themed ANSI values, every extended index 16–255 against independent xterm expectations with explicit boundaries (16=#000000, 28=#008700, 231=#ffffff, 232=#080808, 255=#eeeeee), truecolor/reset behavior, theme/buffer stability and exact rendered pixels:

- `terminalAnsiIndexedColorsRespectThemes`
- `terminalExtendedIndexedColorsMatchXterm`
- `terminalTruecolorAndSgrResetsPreserveRgb`
- `terminalExtendedColorsSurviveThemeAndBufferChanges`
- `terminalIndexedColorsRenderLikeTruecolor`

Receipts: `build/macos/t52-appearance-cocoa.log`, `build/macos/t52-terminal-cocoa.log`, `build/linux/arm64/t52-terminal-xcb.log` (appearance and terminal sections). Linux targets were explicitly rebuilt before the native run (`build/linux/arm64/t52-build.log`).

### Independent OpenSSH/tmux acceptance

`python3 scripts/test-ssh.py --platform cocoa` and `--platform xcb` each passed **all ten owned OpenSSH cases**, retaining the previous nine. The new `sshIndexedColorsThroughShell` prints labeled indexed-28 and equivalent truecolor foreground/background samples over the real shell, checks received cell colors and `#008700` equivalence, then repeats inside an isolated fixture-only tmux session, checking final colors without claiming tmux preserves the original SGR encoding. The fixture reported `TERM=xterm-256color` outside and `tmux-256color` inside; no client/server configuration was changed. The case also verifies session identity, auth count, unchanged XML/trust/preferences bytes and no clipboard access.

Receipts: `build/macos/t52-ssh-cocoa.log` and `build/linux/arm64/t52-ssh-xcb.log`; isolated new-case receipts: `build/macos/t52-ssh-colors-cocoa.log` and `build/linux/arm64/t52-ssh-colors-xcb.log`.

### Final regression gates

Each command passed **54 Rust tests**, **103 Python tests** and exited zero. Full builds passed **24/24 CTest executables**; mock-only builds passed **21/21** on both platforms.

| Command | Receipt |
|---|---|
| `python3 scripts/project.py test` | `build/macos/t52-full.log` |
| `python3 scripts/docker.py test` | `build/linux/arm64/t52-full.log` |
| `python3 scripts/project.py test --mock-only` | `build/macos/t52-mock.log` |
| `python3 scripts/docker.py test --mock-only` | `build/linux/arm64/t52-mock.log` |

No packages were rebuilt, no dependencies changed, and no Windows/AMD64/Wayland SSH parity is claimed. See [indexed/truecolor behavior](ssh-api.md#indexed-and-truecolor-output).

## Scrollback drag selection — T51, 2026-10-11

Continuous bidirectional selection now extends through retained primary history with wheel/trackpad scrolling or a bounded 50 ms edge timer. Absolute row anchors preserve history-only selection and the reading viewport under output/eviction until a selected endpoint is lost. Cursor-only/color-only updates preserve selection; live-grid content changes conservatively invalidate selections touching live cells. Resize, history clear and buffer transitions invalidate them. Release, lost button/grab, focus/deactivation, hide and destruction stop the drag/timer. Alternate-screen selection remains visible-grid only. Clipboard access still requires explicit Copy/Paste, and scrolling/dragging sends no input or persistent writes.

### Native and independent OpenSSH acceptance

Cocoa and Linux ARM64 XCB each passed **121 terminal QtTest checks** and **13 appearance checks**, including setup/cleanup and data rows, with zero failures/skips. The seven new named tests cover exact forward/reverse Unicode Copy, wheel/pixel accumulation, stationary edge scrolling and reversal, lifecycle stopping, history growth/eviction, content/buffer invalidation and independent/disconnected sessions:

- `terminalDragSelectionSpansVisibleRows`
- `terminalWheelAndTrackpadExtendActiveSelection`
- `terminalEdgeDragAutoScrollsThroughHistory`
- `terminalDragAutoScrollStopsOnLifecycleEvents` (eight lifecycle rows)
- `terminalHistorySelectionKeepsStableAnchors`
- `terminalSelectionInvalidatesOnContentAndBufferChanges`
- `terminalDragSelectionRemainsLocalAndSessionScoped`

Receipts: `build/macos/t51-terminal-cocoa.log`, `build/macos/t51-appearance-cocoa.log` and `build/linux/arm64/t51-terminal-xcb-current.log`. Linux targets were explicitly rebuilt before the final native run (`build/linux/arm64/t51-build.log`). The earlier `t51-terminal-xcb.log` ran a stale 107-check executable and is not T51 evidence.

`python3 scripts/test-ssh.py --platform cocoa` and `--platform xcb` each passed **all nine owned OpenSSH cases**, retaining the previous eight. Every case has three QtTest passes including setup/cleanup, zero failures/skips. `sshScrollbackDragSelectionThroughShell` prints 160 deterministic numbered lines through the real shell, waits for the fixture marker and terminal delivery, then checks exact Copy text after angular/pixel scroll and an edge hold. It also checks release/scroll stability, no automatic clipboard access, unchanged session/target/auth count, an unaffected second tab and unchanged XML/trust/preferences bytes. Tests use fake clipboard endpoints and owned fixture teardown, never the host clipboard or user targets. Receipts: `build/macos/t51-ssh-cocoa-rerun.log` and `build/linux/arm64/t51-ssh-xcb.log`; isolated new-case receipts: `build/macos/t51-ssh-selection-cocoa.log` and `build/linux/arm64/t51-ssh-selection-xcb.log`.

### Final regression gates

Each command passed **54 Rust tests**, **103 Python tests** and exited zero. Full builds passed **24/24 CTest executables**; mock-only builds passed **21/21** on both platforms. These offscreen suites retain their native/RHI exclusions and do not replace the native evidence above.

| Command | Receipt |
|---|---|
| `python3 scripts/project.py test` | `build/macos/t51-full.log` |
| `python3 scripts/docker.py test` | `build/linux/arm64/t51-full.log` |
| `python3 scripts/project.py test --mock-only` | `build/macos/t51-mock.log` |
| `python3 scripts/docker.py test --mock-only` | `build/linux/arm64/t51-mock.log` |

### Corrected failures and limits

Fail-first Copy/cursor/timer assertions reproduced the previous clamped endpoints, repaint-only scrollbar route and selection cancellation on every parser notification (B9/V87). Early Cocoa activation failures occurred before selection assertions and were not counted as selection evidence. Component testing then found that Qt suppressed no-button moves without mouse tracking; enabling it lets the lost-button handler stop the timer. The alternate-screen test now explicitly homes its synthetic cursor before writing; no production alternate semantics changed. Focus lifecycle tests dispatch a real `QFocusEvent`. Assertions remain intact.

The first all-case Cocoa run failed native activation before connecting (`build/macos/t51-ssh-cocoa.log`: visible window, inactive window, active application, no modal). After classification, a separate all-case rerun passed without changing the activation requirement or driver. No packages were rebuilt, no dependencies changed, and no Windows/AMD64/Wayland SSH parity is claimed. No user SSH/trust/clipboard changes, agents, commits, original .NET execution or terminal/secret captures were used. Supplied recordings were inspected through task-local contact sheets, not republished. See [selection behavior and limits](ssh-api.md#scrollback-selection).

## Tomorrow Night, terminal shortcuts and session zoom — T50, 2026-10-11

The exact 21-color Tomorrow Night screenshot palette is a color-only preset; Classic remains the default and schema-1 preferences are unchanged. Focused SSH terminals now send conventional Ctrl/C0/DEL and normalized Alt/Meta input independently of native Qt event text, retain application-cursor/special-key encoding, keep Tab/back-tab out of widget focus traversal and protect terminal keys from QAction interception. Local copy/paste/select-all/primary scrollback actions use the existing explicit clipboard route. Full-screen alternate-buffer navigation remains remote.

Ctrl/Command ±/0 changes only the current terminal's bounded 1 pt font offset/reset. Global Apply keeps that clamped offset while publishing other appearance fields. Disconnected/canceled reconnect retains local size; successful reconnect and close/reopen starts at the current global base. Zoom emits no input or persistent write, does not retarget/reauthenticate, and reaches the existing PTY resize path. Other tabs, RDP, editors and the synthetic preview are excluded.

### Final regression gates

Each command rebuilt its targets, passed **54 Rust tests** and **103 Python tests**, and exited zero. Full builds passed **24/24 CTest executables** on both platforms; mock-only builds passed **21/21**. Offscreen native/RHI skips remain explicit; native results below supply separate evidence.

| Command | Receipt |
|---|---|
| `python3 scripts/project.py test` | `build/macos/t50-closure-full.log` |
| `python3 scripts/docker.py test` | `build/linux/arm64/t50-closure-full.log` |
| `python3 scripts/project.py test --mock-only` | `build/macos/t50-closure-mock.log` |
| `python3 scripts/docker.py test --mock-only` | `build/linux/arm64/t50-closure-mock.log` |

### Native keyboard, appearance and independent server checks

Native Cocoa and Linux ARM64 XCB each passed `terminal_test` (**107 QtTest passes**, including setup/cleanup and all data rows) and `terminal_appearance_test` (**13 passes**), with zero failures/skips. Receipts: `build/macos/t50-terminal-cocoa.log` and `build/linux/arm64/t50-terminal-xcb-final.log`. Cocoa's individual result paths are retained inside its receipt. Linux invocation uses the contained development image:

```sh
python3 scripts/test-cocoa.py terminal_test
python3 scripts/test-cocoa.py terminal_appearance_test
python3 scripts/docker.py shell -lc 'python3 scripts/test-native.py --platform xcb terminal_test && python3 scripts/test-native.py --platform xcb terminal_appearance_test'
python3 scripts/test-ssh.py --platform cocoa
python3 scripts/test-ssh.py --platform xcb
```

Both SSH commands passed the original six cases plus `sshControlShortcutsThroughShell` and `sshSessionZoomThroughShell`: **eight independent owned OpenSSH cases per platform**, each with three QtTest passes including setup/cleanup, zero failures/skips. Closure receipts: `build/macos/t50-ssh-cocoa-closure.log` and `build/linux/arm64/t50-ssh-xcb-closure.log`.

The shortcut case verifies fixture-written results of Bash/readline line movement/deletion, kill/yank, Meta word editing, Tab filename completion, interrupt and redraw. An isolated remote tmux socket supplies independently queried window indices/counts and pane counts after Ctrl+B then C/P/N/%; D detaches and the fixture-only server is stopped. The production application retains one session/target/authentication count. The component matrix covers the broader Ctrl/Meta/special-key set; this is not a universal remote-binding claim.

The zoom case verifies server `stty` rows/columns after ± without reauthentication or target/identity change, another tab's unchanged size, and unchanged XML/preferences/trust bytes. Global Apply then preserves the offset; canceled different-credential reconnect retains the old view and successful reconnect uses the current global size. Component/UI tests additionally cover bounds without overshoot, reset/keypad variants, close/reopen, preview/extra-modifier exclusions, exact preset Apply/Cancel/reopen and RDP preservation. `test_native_runners_include_terminal_shortcut_tests` checks both runner target lists.

The fixture adds only test-side tmux (`3.4-1ubuntu0.1` in `build/{macos,linux/arm64}/ssh-fixture-packages.txt`) and a fixed allowlisted `/shortcuts` result oracle. No production dependency or host install was added. Generated credentials/keys, fake clipboard endpoints, loopback-only ports, output secret checks and owned teardown remain in effect. No user account, host clipboard, terminal/secret capture, agent or commit was used.

### Corrected failures and remaining limits

Fail-first tests reproduced native Ctrl text becoming CSI-u/no bytes, Tab focus traversal and unclaimed terminal shortcuts (B8/V86), plus the missing preset/zoom. A special-key matrix initially treated Shift+Insert as remote despite the approved local Paste alias; it now excludes only the documented local aliases and separately proves their fake-clipboard behavior. Additional testing reproduced macOS alternate-screen Command+Home/End being unclaimed during shortcut override; shared navigation classification fixes that interception. Native tests retain their focus requirement: a later Cocoa run failed activation before connecting, and the SSH driver now settles the preceding native unmap and requests activation like the terminal tests. All eight cases subsequently passed on the final production build.

Windows, AMD64 and Wayland SSH remain untested. No packages were rebuilt for T50; older T48 artifacts do not contain this follow-up, and their probes never proved archive-to-server SSH networking. Redistribution remains blocked by license/icon rights. No host environment, HOME, shell startup, original .NET runtime or user SSH trust changes were made. See [keyboard routing and palette values](ssh-api.md#keyboard-routing).

## Embedded SSH2 and terminal appearance — T48/T49, 2026-10-11

These runs cover production MainWindow saved/temporary SSH2, russh 0.64.1 with the selected ring/rsa features, current-thread Tokio 1.48.0, static libvterm 0.3.3 and Qt 6.12.0. XML auth/path inheritance and native SSH2 aliases round-trip; SSH Username is literal and Domain preserved inactive. Version-2 bindings authorize SSH Password accounts without cross-protocol reuse; exact version-1 RDP bindings remain compatible. Private-key mode preserves ciphertext but removes reusable-password authorization and never uses Password as a passphrase. Authentication crosses only the separate session-only cxx bridge; document masters/crypto keys stay C++-owned.

### Final regression gates

The four commands below ran sequentially after the final terminal-margin and native key-picker test-driver corrections. Each rebuilt its targets, passed all **54 Rust tests** and **102 Python tests**, and exited zero. Full builds passed **24/24 CTest executables** on each platform; mock-only builds passed **21/21**. Offscreen suites retain explicit native/RHI skips and do not replace the independent native results below.

| Command | Final receipt |
|---|---|
| `python3 scripts/project.py test` | `build/macos/t48-closure-full-a0e25112328e457d880f3355afb42863.txt` |
| `python3 scripts/docker.py test` | `build/linux/arm64/t48-closure-full-5fabd1fe247443faa69ebc92d69bbb3c.txt` |
| `python3 scripts/project.py test --mock-only` | `build/macos/t48-closure-mock-62bef722ae684dc885b22cc99667c84d.txt` |
| `python3 scripts/docker.py test --mock-only` | `build/linux/arm64/t48-closure-mock-401a0301a5354a32b693a0dd55f20aa2.txt` |

The approved T48 named contract plus T49's seven appearance cases contains 88 distinct test names. All are present in test sources and have individual recorded passes on macOS and Linux ARM64. Current full/mock CTest detail is in each build's `Testing/Temporary/LastTest.log`; those conventional files can be overwritten by later CTest runs. Native/server receipts below retain the required non-offscreen evidence. The earlier `t48-final-verified-regression.txt` and `t48-final-mock-regression.txt` receipts predate the last test-driver corrections and are superseded by these four closure runs.

### Independent native acceptance

`python3 scripts/test-ssh.py --platform cocoa` and `--platform xcb` each passed all six cases through actual production MainWindow windows against the owned OpenSSH/vim fixture, with zero failures/skips:

- `savedSshPasswordThroughShell`
- `savedSshEncryptedKeyThroughShell`
- `temporarySshThroughShell`
- `sshHostKeysAcceptedWithoutPromptThroughShell`
- `sshShellVimUnicodeAndResizeThroughShell`
- `sshIndependentSessionsAndCancellationThroughShell`

Fixture-side markers, saved Unicode vim text and `stty` dimensions distinguish real remote execution from locally displayed text. The resize case applies an 18-point font through the actual appearance dialog and verifies new logical rows/columns at the server without reauthentication/retargeting. Trust decisions check unchanged/replaced cache bytes and server authentication counters; host approval precedes authentication. Independent encrypted-key evidence uses Ed25519, not every promised decoder key type/size. Receipts: `build/macos/t48-ssh-cocoa.txt` and `build/linux/arm64/t48-ssh-xcb.txt`.

The existing owned RDP saved/Quick/XML-password/protected/certificate-rejection/resize/close matrix also passed on Cocoa and Linux ARM64 XCB. Additional `remoteCursorsThroughShell` and `bidirectionalTextClipboardThroughShell` passed with independent server oracles and an injected fake host clipboard. Receipts are `build/macos/t48-rdp-cocoa{,-additional,-session}.txt` and `build/linux/arm64/t48-rdp-xcb{,-additional,-session}.txt`. Deleted vault/legacy adapters were not restored to make these tests compile.

Native `qt_smoke` passed 63 cases on Cocoa with two explicit skips (offscreen-only activation regression and unavailable different-DPR transition); XCB passed 60 with four skips (three AppKit-only cases and unavailable different-DPR transition). These do not waive SSH acceptance: all six SSH cases ran on both platforms. Receipts: `build/macos/t48-cocoa-smoke.txt` and `build/linux/arm64/t48-xcb-smoke.txt`.

### Terminal appearance and session safety

All seven named appearance cases passed natively on Cocoa and XCB: nine QtTest passes including setup/cleanup, zero failures/skips. They check exact classic normal intensity 187 rather than libvterm's 224, explicit bright/default colors, bold behavior, true-color/extended-index preservation, live/history/alternate-screen contents, cursor/selection rendering, logical font resize, draft/Apply/Cancel/Reset semantics, bounded atomic persistence and application across saved/temporary/disconnected/new/reconnecting SSH tabs without changing targets or RDP rendering. Preview uses the real terminal view with hardcoded synthetic text and a no-access clipboard endpoint. No remote terminal capture or real host clipboard was used.

Final Cocoa appearance receipt: `build/macos/native-tests/terminal_appearance_test-b737d46c32424579880fba34920cc3a3/results.txt`. XCB appearance receipt: the appearance section of `build/linux/arm64/t48-xcb-smoke.txt`. Native Config commit/inheritance and captured-owner key-picker checks now pass on both platforms. The final Cocoa receipt is `build/macos/native-tests/property_grid_test-0edebcfae20045c8b58541afcc6a4aae/results.txt`; its earlier activation failure is not a current blocker. The picker driver types into the visible filename editor and verifies the selected-file signal before checking which document saved, rather than relying on Qt's ignored focused-editor `selectFile()` call.

Session checks include `sshCredentialCancelAndLockedOwnerRejectStaleAttach`, `sshEditsAndMovesDoNotRetargetRunningSession` and `sshOutputRejectsRetiredGeneration` on both native platforms. The retired-output case checks queued worker output after page retirement, not arbitrary network-generation injection. Its Cocoa receipt is `build/macos/native-tests/ssh_sessions_test-f2f125b429b34a61a7ca417f487fb206/results.txt`. The terminal-margin test feeds its sample after native geometry settles; no production rendering/assertion policy was weakened. Production code has not changed since the live OpenSSH/RDP and package checks; final changes were test drivers and documentation.

Controlled russh servers and component tests additionally cover PTY/shell refusal, full-output-queue cancellation, stream ordering/bounds, stale trust/auth/output rejection, GUI heartbeat/nonblocking retirement and final worker cleanup. They complement, rather than substitute for, OpenSSH interoperability. Key-file tests cover regular-file/no-follow/FIFO checks, 1 MiB bounds, cancellation between reads, OpenSSH Ed25519/RSA/ECDSA decoding and bounded bcrypt parameters. The RSA2047 regression counts actual significant modulus bits rather than the library's octet-rounded size. Synthetic grammar size checks are not RSA8192 authentication evidence. Individual filesystem/KDF operations and the OS DNS resolver are not guaranteed interruptible; clearing is best-effort, not guaranteed zeroization.

### Relocated development packages

- macOS: `dist/macos/RemoteManager-T48-20261011-macos.zip` and its `.app`. Safe archive inspection and `ditto` extraction preserve/contain framework symlinks. Deep strict ad-hoc signature verification, static `_vterm_new`/`_vterm_screen_get_cell` symbols, no shipped libvterm SDK/runtime library, all 197 locked Rust notice sets and Cocoa shipping smoke passed. Observed dyld loads resolve to the relocated bundle or system, not the build SDK. Probe: `build/macos/t48_package_probe.py`; runtime receipt: `build/macos/t48-package-runtime.txt`.
- Linux ARM64: `dist/linux/arm64/RemoteManager-T48-20261011-final-linux-arm64.tar.gz` and its staging directory. Static libvterm/197-notice audit passed through `build/linux/arm64/t48_notice_probe.py`. Archive relocation, shipping smoke, artifact-local ELF/WinPR/OpenSSL/provider resolution, MD4/SHA256/AES-GCM/BIO/TLS and four software-OpenGL XCB RHI checks passed. Receipt: `build/linux/arm64/t48-package-runtime.txt`. An additional SDK-masked, network-disabled, read-only-root probe also passed; that stricter invocation is recorded in the session tool output, not a separate receipt. Earlier partial/unverified staging directories were preserved.

Package probes exercise startup, linkage, notices and runtime/crypto/RDP rendering, **not packaged SSH-to-server networking**. No package was published. Redistribution remains blocked by the undecided project license and unresolved original-icon rights.

### Evidence limits

Windows, AMD64 and Wayland SSH are untested; earlier AMD64 RDP/package evidence does not prove SSH. Native host-clipboard interoperability, a live different-DPR transition, hardware GPU behavior and performance/SDL parity remain separate unverified work. The owned SSH runner generates synthetic secrets/keys, requires an SSH banner plus fixture oracle readiness, scans output for secrets before publication and cleans up only resources it created. Tests never use user SSH accounts, host sshd changes, `~/.ssh/known_hosts`, secret argv, real host clipboard writes or secret-dialog/editor captures. Source/vector XML compatibility is not original .NET runtime evidence. No agent, commit, push or PR was used for this closure. See [SSH API and limits](ssh-api.md), [password storage](password-vault.md) and [dependencies/licenses](dependencies.md).

## Remote cursors and session text clipboard — T46/T47, 2026-10-10

These results cover the integrated working tree on Apple Silicon/macOS 27.0 and the contained Linux ARM64 SDK, with Qt 6.12.0 and FreeRDP 3.32.1. T46/T47 remain `~`: selected feature checks pass, but the full regression gate and Wayland live acceptance do not. Earlier results below are historical and do not replace those gates.

The desktop follows server-supplied bitmap/default/hidden cursors, including hand, I-beam, resize, crosshair and busy shapes. FreeRDP retains its pointer cache; the adapter owns only its decoded image. GUI scaling preserves the hotspot across presentation size/DPR changes, including a cursor shrunk to one pixel. Letterbox margins, disconnect and reconnect restore the normal pointer. Server position updates never warp the host pointer.

Text sharing is off by default and authorized only for the current session through the tab's **Share text clipboard for this session** action or the existing credentials choice. Saved-password sessions can use the action without a credentials prompt. New/reconnected sessions require fresh consent. Existing host text is synchronized on authorized focus; incoming replies are checked against permission, route and observed host-text revisions. Unicode text is limited to 1 MiB after CRLF expansion. Files, images, HTML and selection clipboard are excluded. An unanswered clipboard request closes the session after a 15-second monotonic deadline, including an outstanding request retained across a permission toggle; no request-slot reuse can misattribute a late response.

| Command/check | Verified result |
|---|---|
| Selected macOS `RemoteManager`, `qt_smoke`, `rdp_cursor_test`, `session_clipboard_test`, `rdp_input_queue_test`, `rdp_session_integration` build | Passed. This is a selected-target build, not a full build. |
| macOS cursor / fake-clipboard / input-queue suites | 12 / 13 / 10 QtTest passes respectively; zero failures/skips. Final selected CTest rerun passed 3/3 executables. |
| Linux ARM64 selected production/helper build and CTest | Passed; cursor, clipboard and input-queue helper executables passed 3/3. |
| Targeted mock-only production/GUI/clipboard build and checks | Passed without FreeRDP linkage. GUI: 6 passes; fake clipboard: 13 passes; zero failures/skips. |
| Cocoa cursor/input/renderer selection | 11 passes, zero failures, one live different-DPR transition skip. Available-display geometry/input checks ran; no second-DPR display was available. |
| Linux XCB and Wayland cursor/input/renderer selection | Each 11 passes, zero failures/skips. Wayland also emitted EGL/Mesa startup warnings; the runner accepted actual RHI presentation/readback and found no null-RHI/resource-leak failure. |
| Owned Cocoa live cursor and clipboard cases | Each 3 passes including setup/cleanup, zero failures/skips; independent cursor-region and exact Unicode/input server oracles passed. |
| Owned Linux XCB live cursor and clipboard cases | Each 3 passes including setup/cleanup, zero failures/skips; both independent server oracles passed. |
| Owned Wayland live command | Failed the first case at `show(window)` before connecting: 2 passes, 1 failure. Clipboard case did not run. The helper requires native exposure/activation; the output does not distinguish which gate failed or prove its cause. |
| Final host Python suite | 90 tests: 88 passed, one failure and one error. New session-runner/oracle, target-validation and native containment tests passed; the two historical migration failures are described below. |
| `python3 scripts/project.py build` | Failed at `tests/rdp_integration.cpp:6`: deleted `VaultService.h` is still included. The old saved/Quick/certificate-rejection/resize transport matrix therefore remains blocked in this working tree. |
| Final `python3 scripts/project.py test --mock-only` | Failed during build at `tests/password_editing.cpp:440,443`: stale `CredentialRequest` aggregate and removed `TransientCredentials::remember`. Rust/Python/CTest stages of this command did not run. |
| Separate contained `cargo test --locked` | Passed 27 Rust tests (12 unit, 2 native projection, 9 profile candidate, 4 profile setting); no doc tests. |

Reproducible selected commands:

```sh
python3 scripts/project.py build --target RemoteManager --target qt_smoke \
  --target rdp_cursor_test --target session_clipboard_test \
  --target rdp_input_queue_test --target rdp_session_integration
python3 scripts/env.py ctest --test-dir build/macos/app --output-on-failure \
  -R '^(rdp_cursor|session_clipboard|rdp_input_queue)$'
python3 scripts/test-rdp.py --target rdp_session_integration --platform cocoa \
  remoteCursorsThroughShell bidirectionalTextClipboardThroughShell
python3 scripts/test-rdp.py --target rdp_session_integration --platform xcb \
  remoteCursorsThroughShell bidirectionalTextClipboardThroughShell
# Fails native window readiness before connection; not an acceptance pass:
python3 scripts/test-rdp.py --target rdp_session_integration --platform wayland \
  remoteCursorsThroughShell bidirectionalTextClipboardThroughShell
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py' -v
```

The common native selection is `clipboardDefaultsOffAndNeverTouchesHost remoteCursorScalesWithDesktopAndDpr remoteCursorRestoresOverLetterbox remoteCursorRejectsRetiredGeneration scanCodesAndFocusRelease rhiPresentsFrameInsideShownMainWindow rhiPresentsFrameAndGrabMatches rhiRepeatedShutdownReleasesResources partialUploadMatchesFullFrame`. Cocoa uses `python3 scripts/test-cocoa.py qt_smoke` followed by those names and `rhiHighDpiPreservesGeometryAndInput`. Linux uses `python3 scripts/docker.py shell -c 'python3 scripts/test-native.py --platform xcb qt_smoke -- <selection>'`, with `wayland` substituted for the second run.

The separate live target avoids restoring deleted vault code. It constructs authorized encrypted XML with a synthetic saved password, injects an in-memory clipboard before MainWindow construction, uses the actual tab action and checks native renderer submission. Host→server paste is verified by an independently read fixture-owned file after canonical newline normalization. Remote→fake-host copy preserves Unicode/newlines; delayed echo, disable, disconnect, fresh reconnect consent and close are checked. Cursor input visits independent server regions and returns to the first bitmap to check actual cache reuse. Certificate approval is explicit and session-only, with Cancel still the default. Expected fixture identity warnings and deliberate transport-cancel messages do not bypass that decision. No test reads/writes the real host clipboard.

Fail-first regressions exposed a scaled hotspot outside a one-pixel image, remote-copy provenance surviving disable, and an empty local format withdrawal disturbing a server-owned copy. The fixes clamp logical hotspots, clear routing caches without erasing clipboard contents, and discard obsolete local ownership on a server offer. Owned-fixture corrections normalize wire CRLF before exact text comparison and release/reclaim Tk CLIPBOARD ownership so repeated copies notify xrdp. Native cursor motion now uses Qt's QWindow test overload; the QWidget helper relied on global pointer movement and missed native motion. No production pointer warp or activation/assertion bypass was introduced.

The Python blockers are `test_cocoa_runner_includes_legacy_conversion`, which still expects deleted `legacy_conversion_test`, and `test_xml_live_native_client_forwards_pair_without_argument_secrets`, which supplies historical `RM_VAULT_TEST_MASTER` rejected by the current XML-only environment allowlist. Retired code/allowlists were not restored. The final mock-full command again encountered stale credential test APIs; selected mock passes do not erase that failure.

Evidence limits: tests inspect this application's native QCursor bitmap/hotspot objects, not a screenshot of the OS pointer. The cursor unit lifetime test explicitly frees/replaces adapter objects; live return-to-hand supplies actual cache-reuse evidence, not forced cache-eviction coverage. The retired-generation test checks queued notification rejection and reset without injecting a nonempty retired-worker cursor. Timeout helper tests cover deadline/serialization, but no intentionally unresponsive live clipboard server was exercised. Equality-based clipboard observation cannot distinguish an unrelated copy of identical text from a delayed duplicate notification. Real native host-clipboard interoperability, Windows, hardware-GPU behavior and a live different-DPR transition remain unverified. These results do not complete T37–T42 or establish SDL parity. No commit, push or PR was made.

Cocoa live receipts: `build/macos/native-tests/rdp_session_integration-177dd78e9c2048d7add7d8a5eede296f/results.txt` and `build/macos/native-tests/rdp_session_integration-73ea7c4db2bf43dc84d7257772de36cb/results.txt`. Cocoa selected GUI receipt: `build/macos/native-tests/qt_smoke-60ef1aaa25f544019f9faa8c02a5acaf/results.txt`. Linux runner output is secret-checked before printing; shared ordinary fixture/client report paths may be overwritten by later runs.

## Linux package runtime repair — T44, 2026-10-10

The VM's Mesa llvmpipe renderer works without a physical GPU. Its old archive omitted Qt's dynamically loaded `xcbglintegrations` plugins and OpenSSL's matching `legacy.so` provider. These omissions caused XCB context/QRhi creation failures and WinPR's MD4 initialization failure. The repair changes packaging and launcher lookup only; renderer architecture, NLA and certificate decisions remain unchanged.

The package now requires and stages both XCB GLX/EGL integration plugins and the legacy provider from the pinned native prefix. Their dependencies enter the existing library closure, all ELF search paths are artifact-relative, and the launcher sets `OPENSSL_MODULES` to its own modules directory. Pinned WinPR already loads both legacy and default providers; no additional OpenSSL activation configuration is needed. Missing required files reject packaging before artifact creation. Existing archives were preserved.

| Command/check | Verified result |
|---|---|
| Four new `test_linux_package_*` regressions | Reproduced missing plugins/provider, absent launcher lookup and missing-runtime acceptance before the fix; all four passed afterward. |
| `python3 -m unittest discover -s tests -p test_package.py -v` | All five runtime-verifier tests passed: environment containment, relocation/required files, unsafe archive rejection, shipping launcher lookup and rejection of skipped/incomplete RHI evidence. |
| Host Python regression suite | All 87 tests passed. |
| Initial `./scripts/linux.sh test --arch amd64` | 31 Rust tests, 82 Python tests and 18/18 CTest executables passed. This run preceded the five verifier tests. |
| Final `./scripts/linux.sh test --arch amd64` and `--arch arm64` | Each passed 31 Rust tests, all 87 Python tests and 18/18 CTest executables. Logs: `build/linux/{amd64,arm64}/t44-final-tests.log`. |
| Relocated AMD64 and ARM64 archives with SDK hidden and network disabled | Each production smoke launch passed. Packaged WinPR MD4/SHA-256 known answers, legacy/default providers, AES-GCM availability, memory BIO and TLS context initialization passed. Loaded WinPR/OpenSSL/provider paths were exclusively inside each relocated artifact. |
| Relocated AMD64 and ARM64 XCB/software-OpenGL presentation | Each passed all four named RHI checks: 6 QtTest passes including setup/cleanup, zero failures/skips. No `No QRhi` or enabled RHI resource-leak diagnostics. |

The presentation checks are `rhiPresentsFrameInsideShownMainWindow`, `rhiPresentsFrameAndGrabMatches`, `rhiRetainsFrameAfterResourceRelease` and `rhiNullFrameClearsPresentation`. They require actual window submission plus pixel/orientation/letterbox, replay and clear checks, not readback alone. `LIBGL_ALWAYS_SOFTWARE=1` is scoped to verification; production does not force a backend. A copied test executable and matching QtTest library enter only the disposable relocated copy, never the shipping archive.

AMD64 archive: `dist/linux/amd64/RemoteManager-runtime-fix-linux-amd64.tar.gz`. Creation command:

```sh
./scripts/linux.sh shell --arch amd64 -c \
  'python3 scripts/env.py python3 scripts/package.py --artifact-name RemoteManager-runtime-fix'
```

For SDK-hidden verification, the matching `libQt6Test.so.6` was copied into `build/linux/amd64/package-test-support/`, alongside an empty `no-sdk/` directory. The container mounts that empty directory over `/opt/remotemanager`, uses a read-only root filesystem and disables networking. The project mount remains writable for generated test data; `HOME` is unchanged and fresh XDG paths contain application data.

```sh
docker --context orbstack run --rm \
  --network none --read-only --tmpfs /tmp:rw,mode=1777 \
  --platform linux/amd64 --user "$(id -u):$(id -g)" \
  --volume "$PWD:/workspace" \
  --volume "$PWD/build/linux/amd64/package-test-support/no-sdk:/opt/remotemanager:ro" \
  --workdir /workspace --entrypoint python3 \
  remotemanager-dev:fe4c9e22e777-amd64 \
  scripts/test-package.py dist/linux/amd64/RemoteManager-runtime-fix-linux-amd64.tar.gz \
  --rhi-probe build/linux/amd64/app/qt_smoke \
  --qt-test-library build/linux/amd64/package-test-support/libQt6Test.so.6
```

The same package creation and SDK-hidden verification passed for ARM64, substituting `arm64` in the paths/image/platform. Its archive is `dist/linux/arm64/RemoteManager-runtime-fix-linux-arm64.tar.gz`; the staged executable matches the final build after applying the package's relative rpath. Retained runtime reports: `build/linux/{amd64,arm64}/t44-package-runtime.log`.

The image tag above identifies this checkout; `./scripts/linux.sh env --arch amd64` prints another checkout's instance/architecture. The verifier owns an Xvfb display with X TCP listening disabled, strips inherited SDK/backend/OpenSSL overrides, audits ELF resolution, and removes its temporary copy after the run. No connection to a user target or host clipboard operation occurs.

The first verification attempt passed crypto but its smoke invocation incorrectly supplied a temporary directory without `Cargo.toml` to `--dev-root`. It failed the existing development-root validation. The runner now uses its already-contained XDG paths and launches `--smoke` without `--dev-root`; production validation was not weakened. The subsequent complete verification passed.

This proves the packaged crypto/runtime and software-OpenGL presentation paths, not a real Windows NTLM login, every NLA policy, hardware-GPU performance or the user's Parrot VM. The user must still retest their VM/server. Certificate mismatch/changed-key warnings remain separate identity-verification issues. Wayland package interaction and Windows remain untested for this repair. These results do not complete T36–T42 or establish SDL parity. Redistribution remains blocked by unresolved project/icon rights; no archive was published, committed or pushed.

## Default-key and optional file-password behavior — T43, 2026-10-09

Verification uses the integrated working tree on Apple Silicon/macOS 27.0 with Qt 6.12.0. Saved passwords now work under mRemoteNG's public `mR3m` default. Password protect independently sets/removes a custom file master; Encrypt entire file changes only inner-XML encryption under the current key. Default-key encryption provides obfuscation, not confidentiality or attacker-resistant binding authenticity. The historical T35 results below describe the earlier custom-master requirement, not the current policy.

| Command/check | Verified result |
|---|---|
| `TMPDIR="$PWD/.dev/macos/cache" python3 scripts/project.py test` | 31 Rust tests, 78 Python tests and 18/18 CTest executables passed, including the final legacy-root regression. |
| XML and document-worker suites | 26 and 18 Qt checks passed respectively; no failures/skips. Default password/full-mode round trips, custom-to-default all-secret reset, import review and atomic failure/cancellation checks passed. |
| Cocoa `property_grid_test`, `multiple_files_test`, `password_editing_test` | 26, 18 and 37 Qt checks passed respectively; no failures/skips. |
| `python3 scripts/test-rdp.py --platform cocoa configPasswordXmlThroughShell multipleConnectionFilesThroughShell` | Config: all four default/custom × password-only/full-inner rows passed (6 Qt checks including setup/cleanup). Multiple files: 3 Qt checks passed. All three independent server authentication/restart/keyboard/mouse ownership checks passed; no failures/skips. |
| Actual `build/macos/app/RemoteManager.app` with a generated contained driver | Three separate native application launches passed: default password save and independent root toggles; custom reopen/Cancel/unlock and master removal; default full-encrypted reopen without a prompt and full-mode removal. |
| `git diff --check` | Passed. No commit/push performed. |

The live Config case saves its first password without a custom-master dialog, optionally sets a custom master through two masked fields, then saves the actual owned fixture password. Each row authenticates, copies XML alone, reopens in a new MainWindow lifetime and authenticates again. Custom full-mode Cancel preserves bytes and hidden metadata. The multiple-file case retains copied portable UUIDs, distinct runtime owners and independent fixture accounts/ports. Its final reconnect locks the primary file, selects the secondary root, unlocks the captured primary owner and checks separate server input. Expected fixture certificate warnings and deliberate disconnect/cancel diagnostics do not bypass the per-session certificate decision.

Named checks covering the changed workflows:

| Contract | Named checks |
|---|---|
| Native crypto/default writes/all-secret reset | `defaultKeyPasswordsAndFullXmlRoundTrip`, `customMasterResetPreservesAllSecretsAndFullMode`, `xmlWriterMatchesOriginalCipherVectors`, `masterEncodingAndKdfBounds`, `xmlTamperingAndParserLimitsReject` |
| Owning worker/atomic outcomes | `defaultKeyPasswordsAreWritableAndReusable`, `defaultKeyImportsNeedReviewNotConversion`, `resetMasterFailurePreservesOwningState`, `resetMasterCancelReconcilesOwningCommit` |
| Independent root controls/open queue | `rootPasswordProtectAndFullEncryptionAreIndependentV71`, `cancelRemovingMasterPreservesFile`, `protectedXmlOpenPromptsOncePerFile`, `multipleProtectedFilesQueueIndependentUnlocks`, `wrongPasswordRetriesBoundedAndCancelDoesNotWrite` |
| Masked input/exact compatible master | `firstSecretSavesUnderDefaultWithoutMasterDialog`, `fileMasterDialogAcceptsCompatibleShortAndLatin1Passwords`, `fileMasterConfirmationRequiresExactMatch`, `configPasswordKeepsRolesAndDraftsSecretFree` |
| Captured Connect/Reconnect/import review | `lockedReconnectUnlocksCapturedOwner`, `connectDuringNativeReviewRetainsCapturedContinuation`, existing copied-ID and stale-account checks |
| Historical adapter preserved | `legacyRootKeepsSingleProtectionControl`, existing property-grid/document/vault/legacy-conversion suites |

Two new regressions exposed implementation defects before their fixes: the master editor silently truncated 4097 characters to 4096, and Connect during an already-open native-password review lost its continuation. The ConnectionFile dialog now retains one overflow character so validation rejects overlong input, and successful review resumes queued captured continuations. Final review also found an unused full-encryption row exposed by the three-argument legacy root loader; a failing regression preceded the narrow loader fix. Synthetic fixture canary/ownership mistakes, an asynchronous test setup race and obsolete default-key rejection assertions were corrected without relaxing crypto, atomic or server-input checks. Direct worker invocations use a canonical project-contained `TMPDIR`; the macOS temporary-directory alias was an invocation issue, not a production storage change.

The generated actual-app driver resides under `build/macos/test-data/t43-app-driver`. It loads through process-scoped `DYLD_INSERT_LIBRARIES` into the unchanged production executable, uses disposable `--dev-root` data and synthetic secrets, requires native exposure/activation and suppresses output reports if either secret appears. It contacts no server and takes no captures. The final checked three-phase report is `build/macos/test-data/t43-app-driver/run-27snda3b/actual-app.log`. Final Cocoa reports are `build/macos/native-tests/property_grid_test-c2ff4976f3f3456cbd9bec2617eb73e2/results.txt`, `build/macos/native-tests/multiple_files_test-b9174b2fa63e4d91ba29b2ea461e7a96/results.txt` and `build/macos/native-tests/password_editing_test-06a49bdfb1df47df829491bce4982bbd/results.txt`; final live reports are `build/macos/native-tests/rdp_integration-8a89b39c7cab4be98cedd44befe35970/results.txt` and `build/macos/native-tests/rdp_integration-7eaa3272f40f426dad822bd30d45f9ed/results.txt`. The standalone driver initially needed the Qt module defines in its compile invocation; no production entrypoint or activation assertion was changed.

Interoperability evidence remains upstream source plus cipher/XML vectors, not an original Windows/.NET runtime test. This change was not newly verified on Linux, Windows, X11 or Wayland; older platform results below remain historical. CBC, alternate ciphers and unsupported XML versions remain rejected. No user connection XML or VM credentials, host clipboard writes or secret-editor/dialog captures were used. These results do not complete the unrelated renderer/performance tasks in the working tree.

## GUI consistency, Options and session fullscreen — T53–T57, 2026-10-11

This verification uses the `t3/gui-sidebar-protocol-fixes` worktree, Qt 6.12.0 and FreeRDP 3.32.1. macOS runs on Apple Silicon/macOS 27.0; Linux ARM64 uses this worktree's separately provisioned pinned Ubuntu 24.04 SDK image. The existing macOS SDK is reused read-only through `RM_SDK_ROOT`; generated builds, Cargo caches, preferences and test data remain worktree-local. The earlier T35 integrated/isolated history below is preserved and is not evidence for these new GUI changes.

### Verification history

| Command/check | Verified result |
|---|---|
| macOS `project.py test` and `test --mock-only`, after compact fullscreen-row/decoder fixes | Each passed 31 Rust tests, 39 Python tests and 14/14 CTest executables. |
| Cocoa `qt_smoke`, `property_grid_test`, `multiple_files_test`, before the modal-release correction | 44, 27 and 14 Qt checks passed respectively; zero failures/skips. Actual AppKit menu export/dispatch and Qt-synthetic Preferences under desktop focus passed; native Cmd+, key-equivalent routing was not yet covered. |
| macOS `preview-ui.py test` | 1/1 offscreen preview CTest executable passed. |
| Linux ARM64 `linux.sh test` and `test --mock-only`, after compact fullscreen-row/decoder fixes | Each passed 31 Rust tests, 39 Python tests and 14/14 CTest executables. |
| Initial Linux preview invocation | Failed before preview execution: `/bin/bash python3 …` attempted to execute the Python binary as a shell script. The preceding SDK/full/mock steps passed. |
| Corrected `linux.sh shell -lc 'python3 scripts/preview-ui.py test'` | 1/1 Linux ARM64 preview CTest executable passed. |
| Complete owned RDP matrices: Cocoa, Linux ARM64 xcb and macOS offscreen, before the modal-release correction | Each passed all 14 functions / 17 functional rows, zero failures/skips. Both fullscreen rows, both XML modes and independent primary/secondary ownership oracles passed. All applicable independent keyboard/mouse/clipboard checks passed. |
| `optionsShortcutSurvivesModalFocusTransfer`, before correction | Failed opening the second Options dialog when the first dialog received the shortcut key release. |
| Modal-release correction, targeted mock regression | The named test, `localShortcutsKeepRemoteKeyStateBalanced` and `nativeOptionsShortcutWithDesktopFocus` passed: 5 Qt checks including init/cleanup, zero failures/skips. |
| Initial post-modal Linux refresh | First stopped at compilation because the new test used a macOS-only helper without a compile guard. After adding the guard, full CTest reached 13/14: the existing XML import test falsely rejected random ciphertext containing `Rm`. Later chained mock/native/preview commands did not run. The structural assertion's first draft then failed compilation because `QStringView::startsWith` needs a typed string argument; it now uses `QLatin1String`. |
| macOS full/mock/preview after production, structural XML and shortcut-test corrections | Each mode passed 31 Rust tests, 39 Python tests and 14/14 CTest executables; preview passed 1/1. A further Cocoa smoke rerun after the explicit shortcut-test activation passed 45 checks, zero failures/skips. |
| Final Cocoa UI and complete live matrix after the modal-release correction | `qt_smoke`, property grid and multiple files passed 45, 27 and 14 Qt checks respectively, zero failures/skips. All 14 live functions / 17 functional rows and applicable independent server oracles passed, zero failures/skips. |
| Initial xcb UI matrix after structural XML correction | Full/mock suites passed 31 Rust, 39 Python and 14/14 CTest checks each. xcb smoke then reported 42 passed, two failed and one AppKit-only skip: two shortcut tests assumed an active top-level and failed their focus prerequisites. Later native targets/preview/live commands in that chain did not run. |
| Targeted xcb shortcut preflight after explicit test activation | All three shortcut regressions passed (5 Qt checks including init/cleanup), zero failures/skips. No production focus policy or focus assertion was changed. |
| xcb UI rerun after shortcut-test activation | Full/mock suites again passed 31 Rust, 39 Python and 14/14 CTest checks each. Smoke passed 44 checks with one expected AppKit-only skip and no failures. Property grid then failed two existing focus-loss tests (25 passed / 2 failed): neither editor had acquired actual focus. Later multiple-files/preview/live commands did not run in that chain. |
| Targeted xcb Config focus diagnosis | Added actual-focus prerequisites reproduced both failures before typing (`edit->hasFocus()` / `line->hasFocus()` false): 2 passed / 2 failed including init/cleanup. |
| Corrected xcb Config and remaining UI/preview | Explicit activation of each test's own top-level plus retained focus assertions passed both targeted cases (4 checks), then property grid 27, multiple files 14 and preview 1/1; zero failures/skips. No production delegate or shared activation-helper change. |
| Final macOS offscreen owned RDP matrix | All 14 functions / 17 functional rows passed after the modal-release correction; zero failures/skips. Both fullscreen rows, both XML modes and all applicable independent server input/clipboard/ownership oracles passed. CPU/transport/input evidence only. |
| Final Linux ARM64 xcb owned RDP matrix | All 14 functions / 17 functional rows passed; zero failures/skips. Both fullscreen rows, both XML modes and all applicable independent server input/clipboard/ownership oracles passed on the owned Xvfb display. |
| Final macOS full/mock and Cocoa Config after the two focus-loss test corrections | Each mode passed 31 Rust tests, 39 Python tests and 14/14 CTest executables; Cocoa property grid passed 27 checks, zero failures/skips. |
| Final Linux ARM64 full/mock after the two focus-loss test corrections | Each mode passed 31 Rust tests, 39 Python tests and 14/14 CTest executables; zero failures. |

### Native Preferences/Config follow-up — B34

User screenshots and the report “CMD + , still opens config” reopened T55/T57 after the preceding acceptance. Config was missing from the native View menu and Preferences showed a dock-style checkmark. Earlier Qt-synthetic shortcut checks did not exercise AppKit's native key-equivalent route and therefore missed this defect; those historical passes are not proof that native Cmd+, was correct.

The new `nativePreferencesAndConfigAreIndependent` regression dispatches Cmd+, through this process's AppKit `NSMenu` `performKeyEquivalent:` method, waits for queued QAction delivery, and separately dispatches native View → Config and application-menu Preferences. The settled baseline first reproduced Config missing from View. Hiding Config and waiting for its action state then reproduced the exact shortcut bug: Config triggered once instead of Options (`toggled.count()` 1, expected 0). An initial immediate assertion had failed before queued QAction dispatch completed; the corrected test waits for either action before checking which fired.

The production correction is one explicit `QAction::NoRole` property on `actionViewConfig` in the shared Designer form, before native menu insertion. Qt's default `TextHeuristicRole` recognizes “Config” as a Preferences candidate, colliding with the explicit Options `PreferencesRole`. Native menus remain enabled; Options/About/Quit keep their intentional native roles. The desktop-focus Cocoa regression now uses the same actual AppKit shortcut route. Shared-form/preview tests assert the roles and Config's presence in native View.

| Follow-up check | Verified result |
|---|---|
| Rebuilt production app; four targeted Cocoa regressions | `nativePreferencesAndConfigAreIndependent`, `nativeOptionsShortcutWithDesktopFocus`, `nativeMenusExportRevisedActions`, `optionsShortcutSurvivesModalFocusTransfer`: 6 Qt checks including init/cleanup, zero failures/skips. Report: `build/macos/native-tests/qt_smoke-9188f9dcc4e943dc98a53d4ea22fd732/results.txt`. |
| Post-B34 macOS full and mock suites | Each passed 31 Rust tests, 39 Python tests and 14/14 CTest executables; zero failures. |
| Post-B34 complete Cocoa smoke | 46 Qt checks passed, zero failures/skips, including actual native Cmd+, under desktop focus, independent Config/Preferences commands and modal release. Report: `build/macos/native-tests/qt_smoke-cb9d08d598f44fa2a85566ae15b8137e/results.txt`. |
| Post-B34 standalone preview, macOS offscreen and Cocoa | Offscreen 1/1 CTest executable; Cocoa 18 Qt checks including actual native View Config export/dispatch; zero failures/skips. |
| Post-B34 Linux ARM64 full, mock and preview | Each suite passed 31 Rust tests, 39 Python tests and 14/14 CTest executables; standalone offscreen preview passed 1/1; zero failures. |
| Post-B34 Linux ARM64 xcb smoke | 44 Qt checks passed, zero failures; two explicit AppKit-only skips (`nativePreferencesAndConfigAreIndependent`, `nativeMenusExportRevisedActions`). Owned Xvfb only; no native macOS proof inferred from this run. |

The native regression uses synthetic, owned temporary data and an event dispatched only to this process's menu, not OS-wide key injection. Its fixture uses the legacy test adapter, but production and preview construct the same canonical Designer menu/actions. The earlier owned RDP matrices remain historical network/fullscreen evidence; this menu-role-only correction does not change transport, storage or credential code.

### Exact RDP-stack rollback and shell-only fullscreen — B35

**Superseded by the SSH merge:** merging the SSH branch (T48–T52) kept that branch's `Views`, `RdpWorker` and `SessionPane`, which carry the QRhi renderer, cursor, clipboard and SSH terminal ownership. The rollback oracle below no longer holds; shell-only fullscreen, Options and Config fixes are retained on top of the SSH stack.

The user reported slower RDP sessions and requested reverting changes to the desktop/session path, retaining fullscreen only as enlargement of the existing widget. No slowdown cause or measured performance recovery has been established. The checkout uses QWidget/QPainter, not QRhiWidget; painting/frame presentation was unchanged even before rollback.

`Views.cpp/.h`, `RdpWorker.cpp/.h` and `SessionPane.cpp/.h` are restored byte-for-byte from `ce1b429`. Empty `git diff ce1b429 --exit-code --` for those six files is the rollback oracle. No equivalent input filter or codec reset is moved elsewhere. Shell fullscreen inserts its controls in the central document layout above the existing tab groups; pane/desktop parents, layout margins/spacing, status and worker identity stay unchanged. Unrelated Config, menu-role, Options and Quick Connect fixes remain.

The earlier tables and correction/test descriptions below are **pre-B35 history**, not post-rollback acceptance. Local Preferences/exit interception, modifier deduplication, local-key bookkeeping, the codec-growth fix and long-status sizing protection were intentionally removed. Ctrl+Alt+Return/Escape/F11 retain baseline remote behavior; fullscreen exits through its top button or shell commands. Automatic resize still follows the captured option and baseline 200ms debounce, so widget enlargement can expose legacy planar growth failures. Baseline long-status sizing is restored; the targeted long-status regression passed on Cocoa, which does not prove all long-error layouts safe.

| Post-rollback check | Verified result |
|---|---|
| Exact six-file diff and macOS production build | Empty baseline diff; build succeeded. |
| Targeted actual Cocoa smoke | 10 passed, 1 failed, zero skips. Shell restoration/identity, captured close, minimize/replacement, visibility, destruction cleanup, baseline remote shortcuts and long-status test passed. `nativePreferencesAndConfigAreIndependent` passed. **`nativeOptionsShortcutWithDesktopFocus` failed**: AppKit accepted the shortcut, but no Options dialog opened with baseline DesktopView focused. Report: `build/macos/native-tests/qt_smoke-2f548b3206354b3b957583aefa1a27d5/results.txt`. |
| macOS full and mock suites | Each passed 31 Rust tests, 39 Python tests and 14/14 CTest executables. |
| Complete actual Cocoa smoke | **45 passed, 1 failed, zero skips.** The same desktop-focus Cmd+, assertion failed. `nativeViewExitsSessionFullscreen`, all shell lifecycle checks, baseline key/focus/frame-ownership checks and independent native Config/Preferences roles passed. Report: `build/macos/native-tests/qt_smoke-2c4940bf6fb54d1b9a0b3e5049ce4e8a/results.txt`. |
| Linux ARM64 full and mock suites | Each passed 31 Rust tests, 39 Python tests and 14/14 CTest executables. |
| Linux ARM64 xcb smoke | 42 passed, zero failures, four explicit macOS/Cocoa-only skips. Owned Xvfb, not a user's desktop/window manager. |
| Shared standalone preview | macOS and Linux ARM64 each passed 1/1 offscreen CTest executable. |
| Owned live Cocoa `fullscreenThroughShell` | Fixed row passed, including minimize/restore, Exit-button/no-key forwarding, same worker/view/session/target, captured close and unchanged settings. **Automatic row failed:** `planar->maxWidth 800 < nSrcWidth 1688`; last frame 800×600, expected 1688×995. Totals: 3 passed / 1 failed including init/cleanup. Report: `build/macos/native-tests/rdp_integration-e68badb11dfe4ea2b0ecbae965d259a3/results.txt`. The runner stopped before its independent fullscreen server-input oracle, so no post-rollback oracle pass is claimed for that function. |
| Owned live Cocoa `automaticResizeHonorsProfile` | Disabled/enabled rows passed: 4 checks including init/cleanup, zero failures/skips. This smaller resize does not cover fullscreen growth. Report: `build/macos/native-tests/rdp_integration-c48470e8bc074dc3810640dedc9f8de7/results.txt`. |
| Owned live Cocoa `fixedResolutionPreservesInputMapping` | 3 checks passed including init/cleanup, zero failures/skips; independent server keyboard/mouse oracle passed. Report: `build/macos/native-tests/rdp_integration-5f125cbca3134d6b9724d568d47865bc/results.txt`. |

The failing native desktop-focus assertion remains enabled; it is not skipped or weakened to conceal the rollback limitation. Preferences remains reachable through the native menu, and Config retains `NoRole`, independent of Options' `PreferencesRole`. Earlier green results do not substitute for post-rollback acceptance. The complete native and live fullscreen suites are not green; T56/T57 remain open in SPEC rather than claiming complete acceptance. The exact requested rollback is delivered with those limitations, not a performance fix.

The fixed-fullscreen app-widget capture at `build/macos/rdp-fullscreen-fixed.png` was opened and inspected: compact shell top controls and an aspect-ratio-scaled owned fixture desktop are visible; the unchanged baseline connected-status label remains. No user desktop, credential dialog or active secret editor was captured. Sanitized per-function logs are retained under `build/macos/test-data/b18-live-vIvHg1/`. No post-rollback complete live matrix, Linux live, Windows, AMD64, Wayland, different-DPR/multi-monitor or performance benchmark was run.

The post-run inventory found 56,052 generated entries owned by UID 501 and 175 symlinks: 172 resolve within this worktree, including explicit container `/workspace` mappings; the three existing `.dev/macos/python/bin/{python,python3,python3.9}` links resolve to the Apple CommandLineTools Python 3.9 prerequisite. No scoped source bytecode or instance-owned containers remain. Both dependency locks and the read-only upstream checkout are unchanged/clean. `git diff --check` and the exact six-file rollback diff passed. No shared SDK byte-baseline claim, global installation, dependency change, user-target test or unrelated-resource cleanup.

### Earlier reproduced defects and corrections (pre-B35)

- Explicit startup/reset docking and signal-blocked explicit visibility synchronization keep Connections/Config usable and Notifications hidden unless requested. Long session status text has plain-text, horizontally ignored sizing and a full tooltip. A parent's hide/minimize does not overwrite user visibility.
- Quick Connect uses the shared compact size policy, the editable address/history combo and one protocol-labelled split-button. Its arrow exposes protocol selection without registering a session. Only RDP is enabled; unavailable SSH/VNC cannot invoke networking or fall back to RDP. The redundant saved-connection control is hidden in production.
- Same scoped XML Config reconciliation adopts stable rows instead of reloading. Effective-protocol field metadata hides irrelevant rows/empty categories in both grid modes, including inherited folder defaults, without deleting hidden overrides. Copied portable UUIDs remain scoped by runtime document.
- The inserted fullscreen row initially required explicit `show()`. Its default vertical policy also stretched it to 398 pixels while its bar needed 34. The height regression now requires a fixed-height row equal to the bar (≤48), at y=0, with the desktop directly below. The row uses Expanding/Fixed; the pane remains in its owning tab.
- Synchronous exit during intermediate native/modal window-state events restored the wrong presentation. Deferred state checks carry a generation and settle before deciding to exit. Native menu tests explicitly activate their own window and wait for menu export rather than assuming prior active-window ownership.
- Live automatic fullscreen resize exposed stale FreeRDP planar decoder bounds: `planar->maxWidth 800 < nSrcWidth 1708`. Legacy `gdi_resize` resized the framebuffer but not the codec context. The worker now validates negotiated dimensions and resets existing codec bounds at post-connect/DesktopResize before GDI allocation/resizing. `desktopCodecBoundsFollowNegotiatedResize` first reproduced the rejection, then decoded exact synthetic RGB across grow/shrink/grow and rejected invalid/oversized dimensions. No advertised codec, transport setting, server implementation or bitmap-bound assertion was relaxed.
- Duplicate nonrepeat modifier downs left remote key state unbalanced; held scancodes now suppress duplicate downs. Final review also reproduced a retained local command key when Options took focus and received its release, swallowing the next Preferences press. Focus loss clears local-command bookkeeping as well as releasing remote-held keys. The modal regression opens Options twice with release delivered to the dialog and checks that a late release is not forwarded remotely. Initial direct test attempts lacked contained `TMPDIR`/window reactivation; an accidental AppKit-helper call offscreen crashed, and its missing compile guard then failed Linux compilation. The test driver now guards both the macOS build and Cocoa QPA; no production activation helper/assertion was changed.

- The existing XML import regression searched the complete ciphertext-bearing document for the two-character string `Rm`. Random Base64 can contain it without any private metadata. `credentialBindingsRejectSwapsAndRequireImportConsent` now parses XML and checks every attribute name for that prefix, rejecting parse errors; its consent/binding/tamper assertions remain intact. No production XML or crypto behavior changed.
- xcb/Xvfb exposed two new shortcut tests that requested widget focus without activating their top-level. Explicit test-window raise/activation fixes those prerequisites. The newly added modal-transfer test already used explicit activation and passed. The next xcb target exposed the same assumption in the existing Config text/spin focus-loss cases: without actual editor focus, the Search click cannot produce the commit they expected. Added actual-focus assertions reproduced that prerequisite failure, then explicit activation of those two test windows corrected it. All focus/write-count/disk assertions remain. These are narrow test setup corrections, not a production delegate, shared activation-helper or desktop-manager/fullscreen implementation change.

### Named GUI regressions

| Invariants | Named checks |
|---|---|
| V89 — dock startup and requested visibility | `notificationsHiddenOnLaunchAndReset`, `firstSessionPreservesDockLayout`, `longSessionStatusDoesNotSqueezeDocks`, `visibilityChecksTrackDocksAndToolbars`, `fullscreenLifecycleAndRequestedVisibility` |
| V90 — compact RDP/SSH protocol selection | `quickConnectProtocolMenuAndCompactControls`, standalone preview layout regression, `quickConnectThroughShell` |
| V91 — stable scoped Config reconciliation | `xmlAutoSavePreservesConfigViewport`, `copiedIdsDoNotShareEditorState`; existing failure/warning/owning-file reconciliation tests |
| V92 — effective-protocol visibility/retention | `protocolVisibilityUsesEffectiveProtocol`, `hiddenProtocolOverridesSurviveRestart` |
| V93 — supported contained Options and Preferences | `optionsApplyCancelAndRestart`, `preferencesStayInExplicitDataDirectory`, `nativePreferencesAndConfigAreIndependent`, `nativeOptionsShortcutWithDesktopFocus`, `optionsShortcutSurvivesModalFocusTransfer` |
| V94 — captured tab-owned fullscreen and local input | `sessionFullscreenRestoresPresentation`, `fullscreenBarTargetsCapturedSession`, `fullscreenMinimizeAndReplacementRestoreSafely`, `fullscreenLifecycleAndRequestedVisibility`, `localShortcutsKeepRemoteKeyStateBalanced`, `fullscreenThroughShell` (fixed/automatic) |
| V45, V94 — bounded negotiated decoder resize | `desktopCodecBoundsFollowNegotiatedResize`, `fullscreenThroughShell` (automatic) |

The live fullscreen test checks exact fixed/automatic framebuffer dimensions, server-observed keyboard/mouse input, minimize/restore with the worker alive, local exit without remote Return, restored chrome, immutable session identity/target and byte-identical saved settings. Reentry/Close removes only the captured connection. Guarded app-widget captures show a compact top-centred bar with the desktop directly below. The automatic-resize fixture's content occupies only part of its enlarged desktop; capture inspection is not a claim of full-desktop content coverage. No host-desktop, other-application, secret-dialog or active secret-editor capture was made.

### Containment and preservation audit history

Before the B34 follow-up, the audit recorded 21,633 generated entries under `.dev`, `build` and `dist` owned by UID 501 and checked 56 symlinks, including six Linux Cargo `/workspace` links mapped to this worktree's container mount. The scoped source scan found no `__pycache__`, `.pyc` or `.pyo`. No automated fixture/client/dev containers remained for worktree instance `0f0860aa8603`. Its two then-observed SDK/fixture images and ARM64 builder were retained; no unrelated resource was removed. The earlier blanket “contained paths” wording did not distinguish existing host Python prerequisite links; the refreshed inventory below explicitly accounts for them.

Both tracked dependency locks are unchanged from HEAD; `dependencies.lock.json` SHA-256 remains `d592693c5f470aa1c3e5890e4f5b3c3b8d2749d37a8fd6511248ff7456d58c77`. The original `mRemoteNG` checkout is clean against HEAD (1,392 tracked files, no ignored/untracked additions). This is not a byte-baseline audit of shared SDK files or `.git` metadata. `git diff --check` passed. Guarded Cocoa fullscreen captures were preserved and inspected under `build/macos/t40-native-final/` before subsequent offscreen output overwrites. No commit, push or pull request was made.

After all B34 follow-up runners completed, a full generated-tree inventory found 56,006 entries owned by UID 501 and 170 symlinks. Of these, 167 resolve within the worktree (including 12 explicit `/workspace` mappings); three venv executable links resolve to the existing Apple CommandLineTools Python prerequisite. An initial overly strict assertion rejected `.dev/macos/python/bin/python3`; inspection established those exact three existing host links, and the corrected audit permits only those paths resolving to that Apple executable. No unexpected escapes or scoped source bytecode were found. Both tracked locks and the original reference checkout remain unchanged/clean. No instance containers remain; the observed ARM64/AMD64 SDK images, ARM64 fixture image and ARM64 builder were retained without cleanup. Image presence is inventory, not AMD64 runtime validation. This inventory does not establish byte-baseline preservation of the shared SDK or `.git`. The production app was rebuilt at `build/macos/app/RemoteManager.app`; no commit, push or pull request was made.

### Platform and scope limits

Cocoa runs prove native AppKit menu/Preferences and tested macOS fullscreen/minimize lifecycle. Linux xcb runs use an owned Xvfb display, not a user's desktop/window manager. Offscreen runs prove CPU/transport/input behavior only. Windows, AMD64 Linux, Wayland/native compositor, multi-monitor/different-DPR transitions and arbitrary desktop-manager behavior were not validated here. No original .NET application was built/run. SSH/VNC networking and SSH private-key controls/storage remain unimplemented; the visibility mechanism does not add them. Options persists only supported nonsecret values in the explicitly supplied application-data path, never connection XML, global QSettings, Notifications visibility or temporary fullscreen suppression.

## Portable XML and multiple files — T35, 2026-10-08–09

T35 verification passed on 2026-10-09. The isolated HEAD-plus-T35 source passed macOS/Linux ARM64 full/mock suites, all eight Cocoa UI targets, complete Cocoa/offscreen/xcb live matrices and both platform preview checks. Independent server input checks passed. The candidate includes only the existing fixed-height QLabel policy as a production layout correction; other compact-layout, renderer, transport and performance changes remain excluded. Integrated-workspace results are labeled separately below and do not complete those concurrent tasks. T35 makes no SDL-parity or all-platform-renderer claim. The XML/crypto implementation and compatibility limits are described in [password storage](password-vault.md).

Earlier Cocoa activation/menu failures remain recorded. The isolated activation preflight failed while the current macOS GUI-session dictionary reported `CGSSessionScreenIsLocked=true`. On the subsequent invocation, that flag was not reported and the unchanged probe passed, including its 3.5-second idle check. No host unlock or assertion bypass was attempted; the state during every older failure was not measured. The earlier isolated full-live run also failed the pre-existing `savedConnectionThroughShell` assertion `status->height() <= status->sizeHint().height()`: HEAD's default vertical layout gave the status label excess height. Adding only the fixed-height policy already present in the working tree corrected that layout defect. The targeted case and subsequent complete live matrices passed without weakening the assertion or server oracle.

| Command/check | Verified result |
|---|---|
| `python3 scripts/project.py test` | 31 Rust tests, 78 Python tests and 18/18 CTest executables passed. |
| `python3 scripts/project.py test --mock-only` | 31 Rust tests, 78 Python tests and 16/16 CTest executables passed. |
| `python3 scripts/preview-ui.py test` | macOS offscreen preview CTest passed. |
| Native preview with `RM_PREVIEW_ASSET_DIR`/contained `TMPDIR`, plus Config-popup capture | 18 Qt checks passed, zero failures/skips; app-owned capture opened and inspected. |
| `./scripts/linux.sh test`, contained `test --mock-only` and preview test | Linux ARM64: full 18/18, mock 16/16 and preview 1/1 CTest executables passed. Full/mock each passed 31 Rust and 78 Python tests. |
| Offline dependency verifier | macOS passed. Linux initially lacked `cxx.crate`; after copying/reusing 11 checksum-verified pinned artifacts from the contained macOS cache, Linux offline verification passed. No install, pin edit or environment mutation by this audit. |
| Cocoa `multiple_files_test`, `password_editing_test`, `legacy_conversion_test` | 12, 24 and 22 Qt checks passed respectively; zero failures/skips. |
| Cocoa XML acceptance before capture refinements | Config password-only/full-inner: 4 Qt checks passed. Multiple files: 3 Qt checks passed. Both server ownership oracles passed; zero failures/skips. |
| Earlier full Cocoa `qt_smoke` | Failed: 10 passed, 5 failed, 1 skipped before abort. Raster/RHI activation, File-menu export and Config activation failed. Remaining targets in that chained command did not run. |
| Earlier Cocoa XML retry after capture refinements | Both Config rows failed `showXmlWindow` before any Config input. Capture refinements did not run; later commands in that chain did not run. |
| Earlier renewed T35 Cocoa prerequisite check | Isolated bare-window probe: 2 passed, 1 failed, zero skips; activation failed. Current GUI-session state reported `CGSSessionScreenIsLocked=true` (numeric scalar), with login complete. Only state flags were read; no account details, desktop capture or host unlock. Full Cocoa/live suites were not repeated with this prerequisite blocked. |
| Subsequent unchanged Cocoa activation preflight | 3 passed, zero failures/skips. AppKit active, Qt active, visible/exposed and native key remained true after 3.5 seconds. The screen-lock flag was not reported on this invocation. |
| Integrated full Cocoa regression before the midpoint correction | `qt_smoke`: 58 passed, zero failures, two explicit skips (offscreen-only case and unavailable live different-DPR transition). Property grid, document, historical vault, moves, multiple files, password editing and legacy conversion: 25, 28, 15, 17, 12, 24 and 22 passed; zero failures/skips. |
| Integrated `python3 scripts/test-rdp.py --platform cocoa` before the midpoint correction | All 14 functions / 17 functional rows passed; zero failures/skips. Both XML modes and independent two-file primary/secondary server ownership oracles passed. All other applicable server input/clipboard checks passed. Guarded app-widget captures opened and inspected: filename roots, Config dock, compact session tabs and live owned desktop visible; no active secret editor/dialog. |
| Separate latest xcb/offscreen XML run, then full/mock regressions | Both Config rows and the multiple-file case passed on each platform; zero failures/skips and independent server oracles passed. Full/mock again passed 31 Rust and 78 Python tests, with 18/18 and 16/16 CTest executables. Guarded Reset Layout captures show both filename-labeled roots. The xcb image also shows the live desktop; offscreen has no native renderer. |
| Earlier scoped reference/containment/cleanup audit | 1,612 protected files unchanged, no additions/removals; 38,804 checked generated entries owned by UID 501; no source bytecode or remaining owned automated fixture/client containers; tracked `Cargo.lock` and `git diff --check` passed. `dependencies.lock.json` has concurrent changes and is excluded from the unchanged-baseline claim. |
| `python3 scripts/test-rdp.py` | All 14 functions / 17 functional rows passed, zero failures/skips. Offscreen CPU/transport evidence only. Independent server input/clipboard checks passed. |
| `python3 scripts/test-rdp.py --platform xcb` | All 14 functions / 17 functional rows passed on the Linux ARM64 client, zero failures/skips. Native-QPA presentation and independent server input/clipboard checks passed. |
| Actual `build/macos/app/RemoteManager.app` | Native launch, toolbar New connection, Config Name autosave and empty masked Password editor/Escape passed. Persisted XML contains the committed name, an empty Password and no `.vault` sidecar. App-owned widget capture opened and inspected. |
| Refreshed isolated macOS/Linux ARM64 full/mock, after midpoint correction | Each platform/mode passed 31 Rust tests, 39 Python tests and 14/14 CTest executables. Candidate excludes concurrent compact-layout, renderer and performance production changes. |
| Refreshed isolated offscreen/xcb XML acceptance | Each platform passed 4 Config checks and 3 multiple-file checks; zero failures/skips, with all three independent server ownership oracles. Cocoa XML/oracles also passed after the correction, before the older full-live layout failure. |
| Refreshed integrated full/mock, after midpoint correction | Each passed 31 Rust and 78 Python tests; full 18/18 and mock 16/16 CTest executables. |
| Refreshed integrated full live Cocoa/xcb/offscreen, after midpoint correction | Each passed all 14 functions / 17 functional rows; zero failures/skips. All three XML ownership oracles and other applicable server input/clipboard checks passed. Offscreen remains CPU/transport/input evidence only. |
| Refreshed offscreen preview | Integrated macOS, isolated macOS and isolated Linux ARM64 each passed 1/1 CTest executable. |
| Final integrated Cocoa XML capture run | Config 4 and multiple files 3 passed, zero failures/skips; all three server ownership oracles passed. Guarded native captures copied to `build/macos/t35-native-final` and opened/inspected. |
| Earlier scoped audit, before the status-policy verification | 1,612 protected files unchanged, no additions/removals; 49,839 generated entries owned by UID 501; no source bytecode or remaining owned automated fixture/client containers for either instance. `Cargo.lock` tracking and `git diff --check` passed. The snapshot's temporary SDK-image tag, created builder and empty Compose network were removed; unrelated resources were not removed. Dependency-lock concurrent changes remain excluded from the unchanged-baseline claim. |
| Final T35-only macOS/Linux ARM64 full/mock, after the status-policy correction | Each platform/mode passed 31 Rust tests, 39 Python tests and 14/14 CTest executables. The generated source matches the selected commit code and excludes concurrent renderer/performance implementations. |
| Final T35-only Cocoa UI matrix | `qt_smoke`, property grid, document, historical vault, moves, multiple files, password editing and legacy conversion passed 31, 25, 28, 15, 17, 12, 24 and 22 Qt checks respectively; zero failures/skips. |
| Final T35-only complete live matrices | Cocoa, offscreen and Linux ARM64 xcb each passed all 13 functions / 15 functional rows; zero failures/skips. Each includes both XML modes, the copied-ID multiple-file case, all three XML ownership oracles and other applicable server keyboard/mouse/clipboard checks. Offscreen remains CPU/transport/input evidence only. |
| Final T35-only preview checks | macOS and Linux ARM64 each passed 1/1 offscreen preview CTest executable. |
| Final isolated Cocoa XML capture run | Config 4 and multiple files 3 Qt checks passed, zero failures/skips; all three independent server ownership oracles passed. Native app-widget captures preserved under `build/macos/t35-native-candidate-final` and opened/inspected. |
| Scoped audit after isolated native captures | 1,612 protected files unchanged, no additions/removals; 50,047 generated entries owned by UID 501; no source bytecode or owned automated fixture/client containers. Created builder and inspected empty source-instance Compose network removed; unrelated resources preserved. `Cargo.lock` tracking and `git diff --check` passed. Concurrent dependency-lock changes remain excluded from the unchanged-baseline claim. |
| Audit after complete isolated verification | 1,612 protected files unchanged, no additions/removals; 50,027 generated entries owned by UID 501; no source bytecode or remaining owned automated fixture/client containers for either instance. `Cargo.lock` tracking and `git diff --check` passed. Temporary SDK-image tag and created builder removed. |

`configPasswordXmlThroughShell` runs without `testMode=true`. It enters a replacement through actual Config key input, accepts the production default-Cancel association review and confirms a custom master through masked inputs. A second replacement becomes the actual fixture password. Password-only and full-inner-XML rows each authenticate without the transient credential dialog, copy **only XML**, reopen in a new MainWindow lifetime, explicitly unlock and authenticate again. Full mode first cancels unlock and checks unchanged bytes/hidden metadata. Saved bytes, draft JSON and tree roles are scanned for synthetic password/master. Source/copy bytes remain unchanged by reopen/connect; neither needs a vault sidecar. Server A/B keys and center/quarter clicks distinguish the initial and reopened connections.

`multipleConnectionFilesThroughShell` retains copied portable file/profile UUIDs while using distinct runtime documents, passwords, masters and ports. Primary/secondary containers have separate filesystems and PAM stores; the secondary shares only the primary network namespace, listens on 3390, and uses X display offset 20 instead of 10. Both host ports are random loopback-only publications. The first file stays password-only; the second uses full-inner-XML protection. Locking the first file preserves both active sessions and leaves the second unlocked. First-file edits do not retarget either session or change second-file bytes. Reopen unlocks each file independently. A fresh primary reconnect uses its owning file's committed font option while the secondary root is selected, without changing the secondary session identity/target.

The independent primary oracle requires A/C/E/G and rejects B/D/F; the secondary requires B/D/F and rejects A/C/E/G. Both require mapped mouse clicks. E proves input while the primary file is locked; F proves secondary input after a primary edit; C/D prove XML reopen; G proves owning-file reconnect. Runner contracts reject missing/crossed keys and missing clicks, require independent input pairs, check primary/secondary environment forwarding and secret suppression, and test reverse cleanup after success, failed secondary startup and failed client execution. No secret is placed literally in Docker argv.

Earlier `savedVaultCredentialsThroughShell` and `protectedConnectionsThroughShell` cases remain explicit historical JSON/vault-adapter regressions. They are not evidence for production XML password storage. Source fixtures and deterministic value vectors establish mRemoteNG format interoperability; no original .NET application was built/run and no Windows runtime interchange was tested.

Two full-live attempts failed older exact source-RGB readiness assertions: fixed-resolution input mapping, then the concurrent graphics-policy case. Fixed-resolution diagnostics recorded `0x1f4f4f` and `0x3d501e`, within the already-existing fixture matcher tolerance (red/blue ≤8, green ≤4). Those readiness assertions now use that matcher. Exact dimensions, pointer mapping, server input, resize behavior, native submission and framebuffer checks remain. No fixture, codec, compression or transport setting was changed. The subsequent complete macOS-offscreen/Linux-xcb run passed; this does not close the separate historical B3/T36 or performance work.

The actual-app driver initially found a hidden retired Name editor, then retained a Config row across the save's rebuild. Draining deferred editor deletion and reacquiring the committed row fixed the driver. Its final launch uses a generated project-contained dylib loaded into the unchanged actual application, not a test executable or production entrypoint patch. It enters no secret, contacts no target, closes the Password editor before capture, and never captures the host desktop. An earlier Cocoa activation failure remains recorded; later targeted passes do not prove its underlying intermittent cause fixed. One final verification chain also stopped because `ui_preview_test` is not a `test-cocoa.py` target; native preview uses its existing direct wrapper with the asset/TMPDIR environment.

A generated bare-`QWidget` diagnostic reproduced the latest activation failure without production controllers or a renderer: Qt and AppKit reported a visible/exposed window, but `NSApp` stayed inactive and the native window never became key. Both current and deprecated diagnostic activation APIs failed. Copied executable bundles, including a unique bundle ID, supplied a valid main-bundle identity but still failed activation; explicit `finishLaunching` also failed. The packaging comparison command exited 0 because LaunchServices completed, but **all three Qt activation checks failed**; it is not a passing test result. The cause remains unproven. No production code, shared activation helper or activation assertion was changed. Diagnostic sources/reports remain under `build/macos/test-data/t35-native-activation` and `build/macos/native-tests/t35_activation_probe-*`. An initial diagnostic build used the wrong moc location; compilation succeeded after using the contained Qt SDK's `libexec/moc`.

The preserved integrated native captures are `build/macos/t35-native-final/rdp-xml-{password-only,full,multiple}.png`; the ordinary output paths can be overwritten by later offscreen runs. All show the app-owned Connections/Config docks, compact session tabs and owned fixture desktop. The multiple-file image distinguishes `first.xml` and `second.xml`; its footer still reads `Saving…`, so it is not an idle-footer assertion. A final isolated Cocoa run also passed both XML methods and all three server oracles. Its native captures were preserved under `build/macos/t35-native-candidate-final` and opened/inspected: both filename-labeled roots, Config, session tabs and owned desktops are visible, with the corrected non-stretching status row. Those captures use HEAD's QWidget/QPainter renderer, not the integrated QRhi implementation. Their multiple-file footer also reads `Saving…`; no idle-footer claim. No host desktop, other application, secret dialog or active secret editor was captured.

The isolated commit-source check uses a generated HEAD-plus-T35 source snapshot under `build/macos/test-data/t35-verified-source`, not a worktree. Its first build exposed a missing explicit `QLabel` include and missing `NativeTestWindow.mm` linkage for `password_editing_test`; the first Python selection also omitted the shared `workspace` helper. Those narrow test/build dependencies were added to the candidate. Its full/mock run then passed 31 Rust tests, 36 Python tests and 14/14 CTest executables each. The isolated Cocoa UI suites passed (31, 25, 28, 15, 17, 12, 24 and 22 checks), but the first live XML case failed the independent center-click oracle despite passing four Qt checks; the chain stopped before later live cases/offscreen. The helper's inclusive `QRect::center()` can undershoot the remote midpoint by four pixels at the permitted 200×150 view size. It now chooses the nearest half/quarter-image point and checks the emitted button-down coordinates against the same three-pixel tolerance before the unchanged server oracle. Three generic owned-client timeout/creation-failure/secret-suppression tests were also retained using RDP inputs; the isolated Python selection now passes 39 tests. No production renderer, transport or shared activation-helper change was included.

After the midpoint correction, the isolated Cocoa Config rows and multiple-file case passed with all three independent server ownership oracles. Its full live chain then failed the older status-height assertion noted above. Targeted isolated offscreen XML acceptance subsequently passed (4 and 3 Qt checks, zero failures/skips) with all three server oracles; this is CPU/transport/input evidence, not native GPU presentation. Fresh isolated macOS full/mock suites each passed 31 Rust tests, 39 Python tests and 14/14 CTest executables. The three resulting offscreen app-widget captures were opened and inspected: filename roots, Config and owned desktop content are visible, with no secret editor/dialog. They also show the baseline's excess status-row space; the multiple-file footer still reads `Saving…`.

The first isolated Linux build stopped before compilation because its absolute macOS reference symlink was outside the container's `/workspace` mount (`PuttySessions.png` missing). Only the generated snapshot was changed: reference inputs were copied locally as read-only files, excluding `.git`, and the contained Linux Cargo cache was retained. Original sources, production code and dependency pins were not edited. After repair, isolated Linux ARM64 full/mock each passed 31 Rust tests, 39 Python tests and 14/14 CTest executables. Both xcb XML methods and all three server oracles passed; the separate preview check passed 1/1 CTest executable. All 1,611 copied reference files are byte-identical and read-only. This snapshot retains HEAD's QWidget/QPainter renderer; its native-QPA XML acceptance is not evidence for the integrated QRhi renderer. The initial isolated full-live failure was an existing layout implementation defect, already exposed by `savedConnectionThroughShell`, not an XML format/interface defect. The 2026-10-09 candidate adds only `status_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);`, retaining every other HEAD line in `SessionPane.cpp`. This is the same policy already present in the working tree. The failing test and its server oracle now pass. No assertion was weakened, and no broader compact-layout, renderer, transport or performance change was included.

### Named XML/file regressions

These existing T31–T34 tests passed in the full/mock reruns. Live XML cases add authentication/input evidence; they do not replace format, parser, fault or commit-boundary checks. Native activation/menu checks also passed in the latest full Cocoa run; earlier failures remain recorded above.

| Invariants | Named checks |
|---|---|
| V27, V46 — compatible crypto/version/master policy | `originalXmlFixturesDecrypt`, `xmlWriterMatchesOriginalCipherVectors`, `masterEncodingAndKdfBounds`, `xmlNamespaceAndVersionRoundTrip`, `firstSecretRequiresCustomFileMaster` |
| V28, V40, V47 — import consent/bindings/association limits | `credentialBindingsRejectSwapsAndRequireImportConsent`, `privateXmlExtensionsRejectPartialInvalidAndUnauthenticatedPayloads`, `nativeImportReviewDoesNotSaveOnUnlock`, `nativePasswordReviewRejectsUnreviewedTargets`, `defaultKeyPasswordsRequireExplicitConversion` |
| V48 — passive XML/secret preservation and unsupported launch settings | `unknownAttributesAndOpaqueNodesSurviveEdit`, `projectionPatchPreservesOpaqueXmlAndInvalidatesMovedCredentials`, `nativeCriticalOptionsRemainPreservedAndNonConnectable`, `masterChangeReencryptsAllSecretKinds` |
| V19, V31, V32, V49 — independent document identity/state | `twoFilesHaveIndependentRootsMastersAndRevisions`, `samePhysicalFileCannotOpenTwice`, `duplicateProfileIdsAreScopedByDocument`, `oneDocumentFaultOrLockDoesNotChangeAnother`, `independentIdleAndUnlockRevisionGates`, `productionRootsAndCopiedIdsAreIndependent` |
| V20, V39, V50 — saved-session/file ownership | `sameIdSessionsReconnectToTheirOwningFile`, `configSavesAndReconnectResolveOwningFile`, `temporarySaveChoosesExplicitFileAndKeepsSessionIndependent`, `closeAndSaveAsReconcileOwningCommit`, `saveAsRechecksSourceAndDestinationAliases` |
| V26, V51 — transient masked secret editing | `passwordEnterFocusEscapeAndClear`, `configPasswordKeepsRolesAndDraftsSecretFree`, `passwordSaveFailureBlocksContinuation`, `lockAndCloseReconcileSecretCommit`, `previewCannotStoreSecrets` |
| V43, V44, V52 — read-only legacy conversion/inactive records | `legacyPairConvertsWithoutChangingInputs`, `conversionPreservesIdsAndInactiveCredentials`, `legacyLookupRequiresCommittedAccount`, `conversionChecksPairAtPhysicalReplacement`, `conversionCancelFaultAndWarningHaveDefinedOutcomes`, `startupDoesNotRewriteLegacyOrXml` |
| V53 — explicit stable-ID repair | `missingXmlIdsRequireExplicitStableRepair`, `xmlRepairConsentKeepsIdentityAcrossFailedReplacement`, `missingIdsRequireExplicitRepairSave` |
| V38, V54 — authenticated all-secret rekey/commit boundary | `masterChangeReencryptsAllSecretKinds`, `xmlRekeyPublishesOnlyAtReplacement` |
| V29, V30, V36, V55 — queued owning-context finalization/warnings | `xmlCommittedProjectionFinalizesOwningCore`, `xmlCommitWarningAndLateCancelAdoptOwningCandidate`, `lockDuringConfigSaveReconcilesOnlyOwningCommit`, `warningContinuationRetainsCapturedProfileAfterForeignEdit`, `documentWarningBlocksOwningContinuation`, `storageAndKdfStayOffGuiThread` |
| V45, V56 — app-only preference/native fallback | `nativeInheritanceAndAppOnlyPreferencesRoundTrip`, `projectionPatchCreatesCompatibleDefaultsAndPreservesNoOpAttributes`, `configSavesAndReconnectResolveOwningFile` |
| V57 — untrusted parser/crypto bounds | `xmlTamperingAndParserLimitsReject`, `xmlPassiveNodeCountsAndPatchDepthAreBounded`, `xmlValueTamperingAndLimitsReject` |

## QRhiWidget repair — 2026-10-08

This historical snapshot repairs desktop presentation only. The T27 results below do not establish acceptance for this renderer or the concurrent password/XML work. At this point, T36 remained in progress because saved/Quick Connect live resize acceptance was failing. The isolated 2026-10-09 results below supersede that blocker without erasing the failure.

The nested Cocoa regression first failed against the original attachment path with `QRhiWidget: No QRhi` and no submission. Constructing the complete SessionPane subtree without a parent, then inserting it into the shown MainWindow, fixes that regression. No forced-RHI environment setting or QPainter fallback is used.

| Current check | Result |
|---|---|
| `python3 scripts/project.py build` | Passed with pinned Qt 6.12.0, GuiPrivate and ShaderTools. Both desktop shader packages compile and deserialize from embedded resources. |
| `python3 scripts/project.py test` | Passed: 31 Rust tests, 22 Python tests, 13/13 CTest executables. Offscreen tests explicitly skip GPU-pixel assertions. |
| `python3 scripts/project.py test --mock-only` | Passed: 31 Rust tests, 22 Python tests, 13/13 CTest executables. |
| `python3 scripts/test-cocoa.py qt_smoke` | 47 passed, 0 failed, 1 skipped. Available-display DPR assertions ran; an actual different-DPR display transition was unavailable. |
| `QT_SCALE_FACTOR=1.5 python3 scripts/test-cocoa.py qt_smoke rhiHighDpiPreservesGeometryAndInput rhiPresentsFrameAndGrabMatches` | 3 passed, 0 failed, 1 skipped. Alternate process scale tested; this is not a live screen-to-screen DPR transition. |
| `python3 scripts/test-rdp.py --platform cocoa fixedResolutionPreservesInputMapping` | Passed with strict frame-revision/window-submission evidence, rendered fixture pixels, letterboxing and independent server keyboard/mouse checks. |
| `python3 scripts/test-rdp.py fixedResolutionPreservesInputMapping` | Passed: offscreen CPU/transport/input checks and independent server keyboard/mouse verification; no GPU-pixel claim. |
| Cocoa and offscreen `savedConnectionThroughShell` / `quickConnectThroughShell` | Failed: pinned FreeRDP planar decoder reports `planar->maxWidth 1024 < nSrcWidth 1124`, then `STATE_RUN_FAILED`. Later frame/resize/reconnect assertions fail. No transport or assertion workaround was applied. |
| Linux real-GL/full/mock | Not run for this repair: cached development SDK lacks qsb and Qt6ShaderTools CMake components. Historical Linux results below do not cover these changes. |

New native regressions cover `rhiPresentsFrameInsideShownMainWindow`, `rhiRetainsFrameAfterResourceRelease`, `rhiNullFrameClearsPresentation`, `rhiTargetChangesPreserveFrame`, all six rows of `rhiRetriesFailedResourceCreation`, `rhiUploadOwnsPixelsAndCoalesces`, `rhiMultipleSessionsRemainIndependent`, `rhiRepeatedShutdownReleasesResources`, and the available-display portion of `rhiHighDpiPreservesGeometryAndInput`. `sessionFramesRejectDisconnectedGenerations` also passes. Native tests fail on `QRhiWidget: No QRhi` and Qt's enabled `QT_RHI_LEAK_CHECK` diagnostics. Presentation evidence precedes framebuffer readback; grab-only rendering cannot satisfy it.

SDK/tool regressions verify a base SDK missing ShaderTools, complete-module reuse, wrong qsb versions and unusable shader output. Native qsb compiled and decoded both project shaders; the actual project build exercises CMake integration. The Cocoa runner checks its in-memory FIFO output for fixture password/master before saving a report. Unit tests verify suppression leaves no results file. Live cases use fresh owned loopback fixtures and synthetic credentials only; transport, certificate defaults and clipboard permissions remain unchanged.

This pass does not establish SDL performance parity, a display-clock scheduler, Linux GPU behavior, a real different-DPR transition, ThreadSanitizer coverage or universal FreeRDP/server compatibility. The dynamic-resize transport failure is a separate acceptance blocker, recorded as B3; T36 must not be marked complete until the required live paths pass.

## Isolated T36 verification — 2026-10-09

T36 remains `~`. The candidate at `build/macos/test-data/t36-candidate` starts from `ce1b429` and selects the full-QImage QRhi renderer, connection-generation guards, ShaderTools upgrade and approved bounded legacy planar-resize repair. It excludes transport wake, GFX policy/decoder, damage, performance and compact-layout changes. The working worker and other mixed implementations remain untouched. No completion commit, package, push or PR was made.

The focused fail-first test reproduced `planar->maxWidth 1024 < nSrcWidth 1124` after the old GDI-only resize. Bounds and reset-failure tests also failed. The repaired helper validates context/storage and dimensions before allocation, resets the public planar codec before checked GDI resize, and observes old-primary EndPaint before replacement. Focused tests then passed. Injected operation failures preserve old storage; an actual GDI allocation failure can destroy it and must end decoding through ordinary worker teardown. The historical B3 failure above remains valid evidence for the original path.

| Candidate command/check | Verified result |
|---|---|
| Fresh existing-base macOS SDK upgrade | Passed. Original Qt preserved; copied SDK independently installed missing ShaderTools. `qsb 6.12.0`, CMake packages and project shader compilation/decoding passed. FreeRDP rebuilt with client/server/FFmpeg/OpenH264 off. |
| `python3 scripts/project.py test` | macOS: 31 Rust, 47 Python and 15/15 CTest executables passed. |
| `python3 scripts/project.py test --mock-only` | macOS: 31 Rust, 47 Python and 14/14 CTest executables passed. |
| Focused `rdp_resize` CTest | Passed after observed fail-first failures; real larger planar decode, exact pixels, shrink, old-primary flush, bounds and injected failures checked. |
| `QT_RHI_LEAK_CHECK=1 python3 scripts/test-cocoa.py qt_smoke` | 46 passed, zero failures, one required live different-DPR transition skip. Renderer readiness/retry rows, resource replay, null clear, stride/source ownership, independent sessions, shutdown and submission-before-readback passed. |
| `QT_SCALE_FACTOR=1.5` targeted Cocoa run | 3 passed, zero failures, same required transition skip. Static scaling is not live screen-transition evidence. |
| Seven remaining Cocoa UI targets | Property grid, document, historical vault, moves, multiple files, password editing and legacy conversion: 25, 28, 15, 17, 12, 24 and 22 passed respectively; zero failures/skips. |
| `python3 scripts/test-rdp.py --platform cocoa` | Complete 14-function / 16-functional-row matrix passed, zero failures/skips. Strict saved/Quick resize/reconnect, fixed-resolution pixels/input and all applicable independent server oracles passed. |
| `python3 scripts/test-rdp.py --platform offscreen` | Complete matrix passed, zero failures/skips, with independent server oracles. CPU/transport/input evidence only. |
| Linux ARM64 SDK image / full / mock | Image built. Full/mock each passed 31 Rust and 47 Python tests; 15/15 and 14/14 CTest executables respectively. |
| Linux xcb live matrix | Failed `protectedConnectionsThroughShell` at `tests/rdp_integration.cpp:479`: file bytes differed from `beforeMove` immediately after accepted drop. Six preceding functions/eight functional rows passed. All seven remaining functions passed in a separate run, including strict Quick resize/reconnect and queued delivery. This does not turn the full matrix into a pass; root cause remains unproved. |
| Linux Wayland live matrix | First function failed both XML-mode rows at `showXmlWindow(window)` (`tests/rdp_integration.cpp:208`); helper requires native exposure/activation. Later functions unrun. No detailed state evidence identifies the failing readiness gate. |
| Linux xcb/Wayland native renderer matrices | Each: 45 passed, zero failures, two skips (macOS-only AppKit check and required live different-DPR transition). All selected renderer/readiness/replay/submission/teardown rows passed; no native parity or live-DPR claim. |
| macOS/Linux preview and offline dependency audits | Each preview passed 1/1 CTest executable; each offline verifier passed. Linux reused seven checksum-verified pinned artifacts from the contained macOS cache. Both installed runtimes report FreeRDP 3.32.1 with client/server/FFmpeg/OpenH264 off; each reports `qsb 6.12.0` and has both ShaderTools CMake packages. |
| Actual `RemoteManager.app` interaction | Driver compiled; direct native launch reached real main, then failed `Actual app window not active`. Stopped before credential input or capture. No activation bypass or host unlock. Actual-app interaction remains unverified. |

`workerFramesPublishOffGuiAndDeliverQueued` passes in both macOS live matrices. It observes off-GUI worker publication and queued GUI delivery without consuming the production mailbox or sending protocol input directly. Blocking the GUI event loop temporarily leaves decoding active while queued delivery waits.

The app-widget integration captures `rdp-xml-multiple.png` and `rdp-widget.png` were opened and inspected: owned fixture desktop, independent filename-labelled roots/tabs and preserved status layout are visible; no active secret editor/dialog. They are not standalone actual-app interaction evidence. The new actual-app driver first failed compilation (missing Qt module defines and complete QPushButton type), then its script-bundle launch produced no report. Those test-driver failures remain in separate receipts; neither is a proved production defect.

Completion gaps: required live different-DPR screen transition skipped; actual-app interaction stopped at native activation; X11 protected-file/drop assertion failed; Wayland live XML window-readiness failed, leaving later functions unrun. Preview/offline and native renderer passes do not replace these missing checks. No private scaling override, display-setting mutation, assertion/tolerance weakening, host clipboard write, user endpoint or original .NET execution was used. Candidate FreeRDP retains the full-frame mailbox, original command order, 16 ms wait and GFX-disabled settings. Its macOS runtime still has pre-existing Homebrew uriparser/jansson transitives; no relocated/self-contained distribution is claimed.

Final scoped audit records 24 selected files, 137 source files and both platforms' executable/build identities in the original workspace's `build/macos/t36-source-runtime-identity.json`; `build/macos/t36-selected-files.json` is refreshed. The original input inventory differs only at the approved SPEC amendment and four scoped documentation files. All 1,611 non-Git reference/prototype files match in bytes/modes. The nested `mRemoteNG/.git` has 90 metadata differences from the retained snapshot; their cause is unproved, and neither original nor snapshot was synchronized/reset. This is not a claim that the entire reference directory stayed unchanged. All 17,320 visited candidate build/SDK/actual-driver entries belong to UID 501; checked app/core/scripts/tests contain no Python bytecode. No owned fixture/client container remains; the index is empty.

## SDL performance work — T37–T42, 2026-10-08

T37–T42 remain in progress. T38 uses an owned WinPR manual-reset event for input/resize/authorized clipboard/stop, with enqueue and reset under one mutex, 1024 queued commands and at most 64 commands per worker drain. The worker reserves a wait slot, checks handle counts/wait failures and waits on network plus input events without the old 16 ms polling timeout. Protocol sends remain on the worker; event destruction follows stop/join. The original 16 ms timeout was not an incoming 60 FPS cap.

T39 now has a capability-derived GFX default and directly linked software OpenH264, with explicit legacy comparison mode. T40 replaces sparse whole-frame production copies/uploads with bounded owned damage batches and partial QRhi uploads. Enabled codec settings do not prove negotiation; live AVC acceptance and matched SDL performance remain unverified. Historical full-frame captures below predate these changes.

```sh
python3 scripts/env.py build/macos/app/performance_trace_test
python3 scripts/env.py build/macos/app/rdp_input_queue_test
python3 scripts/performance-baseline.py --verify build/macos/performance/baseline-3a64bbcdce2445a7ac4eae3f3a63caae
python3 -m unittest discover -s tests -p test_performance.py
python3 -m unittest discover -s tests -p test_dependency_contract.py
python3 scripts/bootstrap-comparator.py --platform macos
python3 scripts/test-performance.py --platform cocoa --workload overlap
python3 scripts/test-performance.py --platform cocoa --workload input
python3 scripts/test-performance.py --platform offscreen --workload sparse --app-baseline
```

Workloads are `idle`, `sparse`, `overlap`, `scroll`, `full` and `input`, each with 180 scheduled steps at 16,666,667 ns. This mode is separate from the ordinary clipboard/resize fixture. The server records scheduled steps and final-scene pixels independently; the input workload additionally requires actual server keyboard/button delivery. Scheduled steps are not received client frames.

The default runner captures the selected app build (current or verified frozen baseline) and a separate instrumented SDL-FreeRDP process on fresh owned fixtures. It checks remote resolution, DPR, logical window size, physical output size, nonempty equal observed-codec sets, zero trace drops and final CPU/GPU pixels. The comparator explicitly disables bitmap caching to match `RdpOptions`' default; a cache-enabled client can receive different legacy orders. The current canonical test uses one remote pixel per physical output pixel, matching upstream SDL sizing. On the tested DPR-2 display, both windows are 512×384 logical and 1024×768 physical for the 1024×768 remote desktop. Earlier forced 1024×768 logical SDL captures remain rejected 2×-output evidence; those sizing hooks were removed. SDL dirty uploads and presentation remain the upstream implementation. The app test waits for native window exposure before connecting.

Passwords enter SDL through `/from-stdin:force`, using a fresh owner-only input FIFO for Cocoa. Only the owned container's SHA-256 certificate fingerprint is accepted. Config/cache/data paths are fresh and project-contained. No password argv/file, certificate-ignore/TOFU, host clipboard operation or user endpoint is used.

Cocoa LaunchServices does not return the child exit status, so a public-path-only wrapper records it after the comparator process exits. FreeRDP's SDL quit path can map deliberate cancellation to `CONN_FAILED` (131). The runner accepts that code **only** with a unique numeric report and unique ordered stop-request, error-free pre-abort, confirmed cancellation and worker-join markers. Ordinary exit 0 still requires completed report/stop/join evidence. Other errors, missing/duplicate/out-of-order evidence and launch timeout fail. The numeric report, independent fixture result and pixel oracles are then checked separately; teardown markers alone do not establish a valid measurement.

Tracing is opt-in, numeric-only, bounded to 4096 records and uses one monotonic nanosecond clock. Raw schema 1 contains 14-column rows. Aggregate schema 2 retains eight 14-column stage rows: stage, count, duration p50/p95/p99/max, bytes, replacements, oldest age p95/p99/max, newest age p95/p99/max. Its seven-column `submission_gaps` row is count, p50/p95/p99/max, fixed diagnostic threshold 50,000,000 ns, count at/above that threshold. Only advancing remote revisions within one geometry and the capture interval contribute; repeated idle repaints and geometry transitions are excluded. The diagnostic threshold is not a performance acceptance tolerance. Historical aggregate schema 1 is parsed explicitly, without invented gap values.

Processing includes protocol work, not decode alone. InputQueue ends at worker-send start, not server response. UploadPreparation is not GPU completion. The current SDL probe records one dirty-upload batch span per window draw, including retained-target draw-command preparation, and sums bytes for every successful dirty upload. Earlier SDL captures measured individual `SDL_UpdateTexture` calls; their preparation durations and sample counts are not directly comparable with batch spans. The trace capacity and zero-drop gate remain unchanged. Qt backing-store submission and SDL's `SDL_RenderPresent` call are distinct stage semantics, neither physical scanout. Final readback is outside measurement and cannot count as window-submission evidence. All nine black/white fixture marker bits are checked: an initial white Tk window must not satisfy completion just because one pixel is white.

The original app-only overlap/input captures are retained under `build/macos/performance`. They establish bounded trace and final native pixel/input checks, not matched SDL parity. The first report containing both clients was rejected for different native geometry, missing SDL codec evidence and trace overflow. Canonical capture `legacy-cocoa-overlap-950b92a2299949e992c762a7cf0f4c54.json` passed both final pixel/model oracles at matched geometry and observed planar codec, but dropped 710 SDL trace records. The next batch-instrumented SDL run failed 15 sampled RGB channels even after settling. Canonical and scaled pixel acceptance remain blocked; one passing final scene does not establish repeatability.

The SDL final-validation hook schedules one 200 ms post-measurement event after the exact completion marker. It only presents/readbacks the retained target, never uploads/reconstructs the whole desktop. Its allocated event ID is beyond upstream's fixed user IDs and is polled separately after ordinary window events; dialog-result events stay excluded. The independent final CPU/model oracle remains required. This hook has both passing and failing native pixel evidence and is not a fix for SDL's intermittent retained-pixel mismatch.

Before T38 worker integration, the unoptimized macOS executable and runtime dependencies were frozen in `baseline-3a64bbcdce2445a7ac4eae3f3a63caae/PerformanceBaseline.app`. `scripts/performance-baseline.py` hashes bundle files/links and relevant sources, rejects escaping links, audits all Mach-O architectures, removes mutable project SDK search paths and ad-hoc signs the private bundle. `--replay-baseline` verifies hashes and uses its own Qt plugins. The contained live input replay launched but failed strict final submission (presented revision 59 versus 63); that is not accepted native replay evidence. Earlier snapshots without the contained-runtime marker are rejected by the current verifier.

T38's queue/event tests cover actual idle waits, reset/enqueue interleaving, every command kind, command ordering/releases, bounded drains, coalesced resize/clipboard, stop/join lifetime and creation/set/reset failures. Full and mock-only regression suites pass. Owned offscreen `certificateAcceptedWithoutPromptAndReconnect`, `panelCloseStopsAllWorkers` and `fixedResolutionPreservesInputMapping` pass. Transport-only input capture `legacy-offscreen-input-9e06776ec0374c2e99be44eab906394b.json` has 92 InputQueue samples, p50 308,083 ns, p95 522,792 ns, p99/max 740,208 ns and zero drops. This is not a matched native before/after comparison.

Native input captures lost exposure/activation during the workload, despite beginning active; the recorded state was visible=1, exposed=0, active=0, renderer-ready=1. The runner now rejects loss of exposure/activation during motion instead of lengthening submission timeouts or forcing repaint. AppKit test activation is also gated on Cocoa; the actual offscreen crash regression is fixed and covered by `nativeActivationIgnoresOffscreenHandle` plus the owned offscreen input run.

Matched reports retain explicit blockers and return 2 when capture completes but acceptance is missing; app-only capture returns 0 with its blockers preserved. A client/oracle failure returns 1. During-motion pixel sampling, repeated matched workload baselines and predefined tolerances remain outstanding. Complete strict fixed/resize acceptance, live AVC, Windows and matched Linux X11/Wayland performance remain open. The Linux saved/Quick shell cases below now have native-QPA evidence. No task is marked complete from compilation or final static pixels alone.

### Current damage/GFX verification snapshot

The legacy planar allocation repair is covered by `legacyPlanarResizeResetsAllocation`, including decoding beyond the old width. The offscreen `legacyServerStillConnects(explicit-legacy)` row also passes at fixed 1280×720 and resizes to 1124×768. This does not replace the required production-shell/native resize acceptance.

The owned Ubuntu 26.04 xrdp fixture is **GFX capable**, contrary to an earlier legacy-only description. Independent failure diagnostics record GFX version `0x000a0701`, output mapping and `starting gfx rfx pro codec session`. The current client trace records Planar (3) and Progressive (10). Missing Progressive/ProgressiveV2/ClearCodec/Alpha mappings were reproduced and fixed, with numeric parser regressions. No live AVC/H.264 negotiation was observed. The strict GFX 1280×720 corner-pixel assertion still fails; no codec, compression, fixture or assertion workaround was applied. The earlier null-frame symptom is not independently resolved by this evidence.

| Command/check | Verified result |
|---|---|
| Integrated `python3 scripts/project.py test` / `test --mock-only` | Passed: 17/17 and 15/15 CTest executables; each 31 Rust and 39 Python tests. This snapshot predates the additional runner/geometry/pacing regressions below. |
| `desktop_updates_test` / `rdp_graphics_test` | 10 and 14 passed, zero failures/skips. Owned pixels, disjoint/overlapping coalescing, full recovery, bounds and exact two-surface GFX damage union. |
| Targeted Cocoa renderer run | 19 passed, zero failures, one different-DPR transition skip. Partial/full readbacks equal; sparse upload bytes below full image; resource replay, readiness, clear, MSAA, independent sessions, shutdown and captured submission/grab evidence pass. |
| Cocoa `damageUpdatesRejectIncompatibleGeometry partialUploadMatchesFullFrame partialUploadSurvivesResourceFailure presentationPacingStaysEventDriven presentationBurstKeepsNewestRevision screenshotCannotCountAsPresentation` | 8 passed, zero failures/skips. Same-epoch resize rejection first failed, then passed after the guard fix. No scheduling change was made. |
| Full `python3 scripts/test-cocoa.py qt_smoke` | Failed before the new partial-upload test: missing native File menu in two cases, then fatal Config-window activation failure; 9 passed, 3 failed, 1 skipped. The targeted renderer pass does not erase this result. |
| Cocoa app-only sparse workload | Rejected: lost exposure/activation during motion, no accepted numeric report. No forced repaint, timeout extension or activation-gate bypass. |
| Offscreen app-only sparse workload | Final CPU/model/independent fixture oracle passed, observed Planar, zero drops. 64 Copy samples total 37,748,736 bytes. Upload/submission samples are zero; this is transport/CPU evidence only. |
| Relocated private T39 package smoke | Passed with packaged Cocoa plugin, fresh contained data root, bundled FreeRDP/OpenH264 loads, no development or original-dist runtime paths. The package predates T40; no distribution clearance is claimed. |

Sparse report: `build/macos/performance/legacy-offscreen-sparse-47d0ad72e9c2428b889a7e8becbc3b2c.json`. Copy p95/p99 were 308,542/335,917 ns; GUI application p95/p99 were 810,958/974,167 ns. These are one offscreen run, not accepted matched before/after thresholds. Native unit readbacks prove partial-upload correctness; they do not establish clean motion or SDL parity.

Cocoa policy forwarding also had a reproduced runner defect: `RM_PERFORMANCE_POLICY` was omitted from LaunchServices arguments. Its named regression now passes. Earlier native GFX requests through that runner cannot be treated as GFX-policy evidence. The direct offscreen typed Progressive evidence is unaffected.

Current logs: `damage-{project-tests,mock-tests,native-renderer,pacing-native,live-codecs,sparse-cocoa,sparse-offscreen}.log`, `decoder-relocation-final-smoke.log`, and per-launch `native-tests/*/results.txt` under `build/macos`. Server diagnostics are secret-checked before saving.

### Linux native-QPA verification, 2026-10-08

The corrected SDK image and Linux ARM64 application build pass. Fresh bootstrap now includes the shader sources, installs GLib before `qsb`, and gives shader validation a child-scoped bundled ICU search path. The installed Qt6GuiPrivate dependency files require xkbcommon headers/library; the reproduced missing-XKB configuration failure is fixed with `libxkbcommon-dev`. Missing optional Vulkan headers are not claimed as the fatal cause.

`scripts/test-native.py` owns an Xvfb display or a private Weston headless GL compositor. Runtime directories are owner-only and project-contained, inherited display variables are removed, and test temporary files stay under `build/linux`. Fixture passwords are excluded from display-server environments. Native RDP/performance clients run in an owned SDK container sharing only the owned fixture's network namespace, using loopback port 3389. App credentials are forwarded by environment names, never command-line values. SDL receives one bounded password line through Docker's interactive stdin; it has no password environment/argv value and uses fresh contained config/cache/data paths. The client has project/instance labels; successful creation is followed by attach and explicit cleanup, including on client timeout. Creation failure does not authorize removal. Secret checks precede output publication.

| Check | Verified result |
|---|---|
| X11 and Wayland full `qt_smoke` | Each 55 passed, zero failures, 4 explicit skips: two AppKit idle rows, AppKit menus and a live different-DPR display transition. Current-DPR pixel/input checks ran before the transition skip. Both used real OpenGLES2/llvmpipe, including independent sessions, MSAA, late attachment, resource replay and shutdown. Repeating each full suite with child-scoped `QT_SCALE_FACTOR=2` also yielded 55 passed and the same 4 skips; static scaling is not a live display transition. |
| X11 and Wayland damage/recovery/pacing/shutdown selection | Each 9 passed, zero failures/skips. Real partial/full pixel readbacks, sparse resource-failure recovery, geometry rejection, idle/burst/grab attribution and repeated shutdown passed. |
| `rhiReportsNativeDriver` on X11 and Wayland | Each 3 passed. QRhi OpenGLES2 backend; Mesa llvmpipe LLVM 20.1.2, OpenGL 4.5, Mesa 25.2.8. These are software-rendered native-QPA checks, not hardware GPU evidence. |
| Saved and Quick Connect through the production shell | Both cases passed on X11 and Wayland, each 3 QtTest passes; independent server keyboard/mouse checks passed. |
| `fixedResolutionPreservesInputMapping` | Failed unchanged strict corner pixels on both backends at 1280×720. Near colors were observed, but the assertion was not relaxed and input-mapping acceptance was not reached. |
| X11 legacy sparse app capture | Final CPU/GPU/model/independent fixture pixels passed; Planar (3), zero trace drops, 180 server scheduled steps. No matched SDL or unoptimized baseline. |
| Wayland legacy sparse app capture | Rejected before connection: native window did not become active. No numeric report and no activation bypass. |
| X11 and Wayland `automaticResizeHonorsProfile` / `closeShellDuringResize` | Each backend: 4 and 3 QtTest passes, respectively; zero failures/skips. Both automatic-resize profile rows and closing during resize passed. This does not erase the strict fixed-resolution pixel failure. |
| X11 and Wayland `liveFramesInputClipboardResizeAndReconnect` | Each 3 passed, zero failures/skips. Two worker generations, renderer pixels, synthetic transport input/clipboard, resize, retained-image lifetime and stop/join passed. No host clipboard operation was used. |
| X11 certificate rejection / panel shutdown / secret-output tests | `certificateAcceptedWithoutPromptAndReconnect`, `panelCloseStopsAllWorkers`, `transientSecretsExcludedFromLogs`: each 3 passed, zero failures/skips, using the explicit owned-client cleanup path. |
| Current Cocoa driver/damage/recovery/pacing/shutdown selection | 10 passed, zero failures/skips; QRhi Metal on Apple M5. Separate activation failures remain unresolved. |
| Fresh Linux SDL comparator and runtime audit | FreeRDP 3.32.1 version check passed. SDL enumerated X11 and Wayland. ELF load audit selected comparator-local FreeRDP/WinPR/SDL/FreeType, selected SDK QtCore/OpenSSL/zlib, and system display libraries; no unresolved loads. |
| X11 matched legacy sparse diagnostic | App and SDL final pixels/model/independent fixture checks passed, matching 1024×768 remote/window/output, DPR 1, Planar (3), zero trace drops and 180 scheduled steps each. Exit 2 retains missing acceptance evidence; no parity claim. |
| Wayland SDL-only legacy sparse diagnostic | Final native pixels/model/independent fixture checks and ordered shutdown passed; 1024×768, DPR 1, Planar (3), zero drops. App activation still blocks a matched Wayland pair. |

Fresh comparator bootstrap uses the selected SDK paths and its own contained download cache. `libx11-dev`, `libxext-dev`, and `libwayland-dev` repair the reproduced SDL native-window prerequisite failure. Child runtime search paths prefer the separate comparator libraries; no application SDK library is replaced. Native execution forces the requested SDL video backend and creates/removes owner-only config/cache/data directories. Passwords remain on stdin, including Docker attach; startup arguments never contain them. Unit coverage also checks bounded single-line input, config cleanup, cancellation status, ownership, timeout cleanup and secret suppression.

The first matched report is `build/linux/arm64/performance/legacy-xcb-sparse-0bd8de31b1d646a18589df86e212dfc8.json`. App Copy: 62 samples, 36,569,088 bytes, p95/p99 292,918/383,669 ns. App UploadPreparation: 63 samples, 37,158,912 bytes, p95/p99 248,585/1,132,132 ns; SDL: 90 samples, 41,312,256 bytes, p95/p99 9,537,565/22,158,318 ns. WindowSubmission p95/p99: app 12,219,833/14,702,142 ns; SDL 5,549,913/8,791,727 ns. Submission-gap p99: app 59,305,070 ns; SDL 1,193,415,042 ns. These are one-pair stage diagnostics with different client/build paths, not a speedup or threshold acceptance. During-motion pixel validation and scaled-output acceptance remain missing. SDL failure now preserves the independently verified app aggregate and reports a blocker instead of discarding app evidence.

Wayland SDL-only report: `build/linux/arm64/performance/legacy-wayland-sparse-sdl-diagnostic.json`. Its successful comparator run does not bypass or satisfy the app's activation gate.

The sequential X11 diagnostic matrix covered all six workloads:

| Workload | Result |
|---|---|
| Sparse, three repetitions | All app/SDL final checks passed with matched geometry, Planar and zero drops. App submission p95 ranged 12.07–15.15 ms; SDL 5.55–6.59 ms. No accepted repeatability/threshold result. |
| Idle, full-frame, input | Each app/SDL final check passed. Input also passed the independent fixture keyboard/mouse oracle. App recorded 172 InputQueue samples, p95/p99 836,085/1,175,044 ns; SDL has no comparable InputQueue instrumentation, so this is not a before/after input-latency result. |
| Overlap | App final checks passed; SDL retained-target check failed at DPR 1: first mismatch `(816,64,blue,32,72)`, 18 channel mismatches, maximum difference 216. No SDL numeric report accepted; app evidence retained. |
| Scroll | App final checks passed; SDL retained-target check failed at DPR 1: first mismatch `(1008,32,blue,0,72)`, 6 channel mismatches, maximum difference 72. No SDL numeric report accepted; app evidence retained. |

Measurement intervals also need repair before accepting a baseline. The app starts after its first complete-size CPU image and stops after final presentation plus a 150 ms drain; SDL starts at its first queued paint and stops when the completion marker is captured. Neither interval is aligned to the fixture's motion-start marker, and the fixture has a 1200 ms startup pause. Diagnostic gaps therefore cannot be classified as motion stalls without phase attribution. The runner now reports `client measurement intervals are not aligned to workload markers` as an explicit comparison blocker. During-motion validation, a retained-target correctness diagnosis and aligned repeated unoptimized/current comparisons remain pending; no production pacing change was justified.

Matrix logs are `build/macos/damage-linux-xcb-matched-{sparse-2,sparse-3,idle-1,overlap-1,scroll-1,full-1,input-1}.log`; each names its retained JSON report. The logs and aggregates are secret-checked, project-local evidence, not publication.

Earlier X11 app-only report: `build/linux/arm64/performance/legacy-xcb-sparse-d60a0fdcab5846e7853b4727fa167d47.json`. Copy: 67 samples, 39,518,208 bytes, p95/p99 288,001/405,710 ns. UploadPreparation: 65 samples, 40,894,464 bytes, p95/p99 167,500/971,504 ns. WindowSubmission: 65 samples, p95/p99 13,217,764/20,315,627 ns. Advancing-revision submission gaps: 64 samples, p95/p99 57,026,990/59,819,127 ns; 13 reached the diagnostic 50 ms threshold. These measurements do not establish improvement against an unoptimized baseline, clean motion, physical scanout or SDL parity.

Exact host-side live commands:

```sh
python3 scripts/test-rdp.py --platform xcb savedConnectionThroughShell quickConnectThroughShell fixedResolutionPreservesInputMapping
python3 scripts/test-rdp.py --platform wayland savedConnectionThroughShell quickConnectThroughShell fixedResolutionPreservesInputMapping
python3 scripts/test-performance.py --platform xcb --workload sparse --policy legacy --app-baseline
python3 scripts/test-performance.py --platform wayland --workload sparse --policy legacy --app-baseline
python3 scripts/test-performance.py --platform xcb --workload sparse --policy legacy
python3 scripts/test-rdp.py --platform xcb automaticResizeHonorsProfile closeShellDuringResize
python3 scripts/test-rdp.py --platform xcb certificateAcceptedWithoutPromptAndReconnect panelCloseStopsAllWorkers transientSecretsExcludedFromLogs
```

A matched diagnostic with unresolved acceptance blockers exits 2. The SDL-only Wayland diagnostic above called the same owned `capture('sdl', ...)` path directly; it is not a matched performance command.

Contained renderer commands run in the SDK container:

```sh
python3 scripts/test-native.py --platform xcb qt_smoke damageUpdatesRejectIncompatibleGeometry partialUploadMatchesFullFrame partialUploadSurvivesResourceFailure presentationPacingStaysEventDriven presentationBurstKeepsNewestRevision screenshotCannotCountAsPresentation rhiRepeatedShutdownReleasesResources
python3 scripts/test-native.py --platform wayland qt_smoke damageUpdatesRejectIncompatibleGeometry partialUploadMatchesFullFrame partialUploadSurvivesResourceFailure presentationPacingStaysEventDriven presentationBurstKeepsNewestRevision screenshotCannotCountAsPresentation rhiRepeatedShutdownReleasesResources
```

Cocoa activation remains unresolved. A plain idle QWidget initially stayed visible/exposed/active for 3.5 seconds. The expanded raster/QRhi diagnostic subsequently failed both rows at initial activation; it does not isolate a renderer or transport cause. The updated full macOS suite also timed out in `connection_moves` and `document_service` under their unchanged 90-second contracts. Their first unfinished cases passed isolated in 231 ms and 142 ms. That narrower evidence does not erase the full-suite failures or establish their cause. A Linux full run passed 17/17 CTest executables; its mock run then caught the newly added client-cleanup regression before the fix. The stable post-fix rerun passed Linux full/mock 17/17 and 15/15 CTest executables and macOS mock 15/15; each passed 31 Rust and 60 Python tests. The macOS full rerun no longer timed out, but failed `ConnectionXmlTest::credentialBindingsRejectSwapsAndRequireImportConsent` at `!original.contains("Rm")`: that assertion searches random base64 ciphertext as well as XML attribute names. The unrelated XML test/code was left unchanged. That macOS snapshot failed. After the comparator changes settled, macOS and Linux full/mock reruns passed: each 31 Rust and 70 Python tests, full CTest 17/17 and mock CTest 15/15. The final measurement-interval blocker/assertion also passed all 70 Python tests on both hosts. The earlier intermittent XML assertion and activation failures were not repaired by these reruns and remain recorded; a later pass is not their root-cause fix.

## Historical T27 commands and results

Run from the checkout with the existing contained SDK and OrbStack:

```sh
python3 scripts/project.py test
python3 scripts/project.py test --mock-only
python3 scripts/test-cocoa.py property_grid_test
python3 scripts/test-cocoa.py qt_smoke
python3 scripts/test-cocoa.py document_shell_test
python3 scripts/test-cocoa.py vault_shell_test
python3 scripts/test-cocoa.py connection_moves_test
python3 scripts/preview-ui.py test
QT_QPA_PLATFORM=cocoa RM_PREVIEW_ASSET_DIR="$PWD/build/macos/ui-preview/assets" \
  TMPDIR="$PWD/build/macos/ui-preview/test-data" \
  python3 scripts/env.py build/macos/ui-preview/ui_preview_test
./scripts/linux.sh test
./scripts/linux.sh test --mock-only
./scripts/linux.sh shell -c 'python3 scripts/preview-ui.py test'
python3 scripts/test-rdp.py
python3 scripts/project.py run --smoke
./scripts/linux.sh run --smoke
python3 scripts/verify-dependencies.py --offline --platform macos
git ls-files --error-unmatch Cargo.lock
git diff --check
```

The final T27 matrix completed with exit 0 after the cross-platform Return/keypad-modifier fixes:

| Check | Result |
|---|---|
| macOS full and mock-only | Each: 25 Rust tests, 15 Python tests and 8/8 CTest executables passed. Full Qt: 152 passed; mock Qt: 137 passed. Each has one expected AppKit-only skip. |
| Cocoa production | Property grid 25, shell 31, document shell 28, vault shell 15, connection moves 16 passed; zero failures/skips. |
| macOS preview | Offscreen 1/1 CTest executable and Cocoa 17 Qt tests passed; zero failures/skips, including extracted images. |
| Linux ARM64 full and mock-only | Each: 25 Rust tests, 15 Python tests and 8/8 CTest executables passed. Full Qt: 152 passed; mock Qt: 137 passed. Each has one expected macOS-only skip. |
| Linux ARM64 preview | 1/1 CTest executable, 17 Qt tests passed; zero failures/skips. |
| Owned live RDP | All 11 functions passed, including both automatic-resize rows; 12 functional cases, zero failures/skips. Server input checks passed, including protected restart B/quarter. |
| Native/Linux smoke and offline verifier | All exited 0. |
| Final reference/containment/cleanup audit | Passed; details below. |

Qt totals include each executable's initialization/cleanup checks; the live functional count excludes those checks.

The offscreen shell deliberately skips AppKit-only `nativeMenusExportRevisedActions`; its Cocoa run supplies that evidence. Mock-only excludes real-adapter entry points at compile time. Rust doc-tests contain no tests. Cocoa preview needs `RM_PREVIEW_ASSET_DIR` to run extracted-image decoding; omitting it skips that optional check, so the final invocation supplies it.

Reports stay under platform build directories:

- `build/macos/t27-native.log`, `t27-mock.log`
- `build/macos/t27-cocoa-{property,shell,document,vault,moves}.log`, with per-launch `native-tests/*/results.txt`
- `build/macos/t27-preview.log`, `t27-preview-cocoa.log`
- `build/macos/t27-linux.log`, `t27-linux-mock.log`, `t27-linux-preview.log`
- `build/macos/t27-live.log`, `rdp-integration.log`, `rdp-fixture-packages.txt`
- `build/macos/t27-launch.log`, `t27-linux-launch.log`, `t27-dependencies.log`, `t27-audit.json`
- CTest details: each app/app-mock/ui-preview build's `Testing/Temporary/LastTest.log`

## Named invariant checks

| Invariant | Executable evidence |
|---|---|
| V1: no profile passwords | `persistence_roundtrip_stable_ids_and_no_secrets`, `corrupt_unknown_version_and_secret_fields_are_preserved`, `saveAsConnectionIsAtomic`; live profile-byte scans |
| V2: no logged secrets | `secret_debug_is_redacted`, `transientSecretsExcludedFromLogs`, `test_rdp_runner_rejects_secret_output`; runner rejects password/master before output |
| V3: stable IDs | Persistence roundtrip, `profileCrudInheritanceRestartAndTabLifecycle`, `dragFolderMovesSubtreeWithStableIds`, protected/live restarts |
| V4: explicit inheritance | `inheritanceWithoutSourceLabels`, `explicitEmptyOverrideSurvivesRestart`, `newSettingsInheritAndRoundTrip`; inherited live settings |
| V5/V6: worker and queued GUI delivery | `vaultOperationsStayOffGuiThread`, `documentOperationsStayOffGuiThread`, `savedConnectionThroughShell`, `liveFramesInputClipboardResizeAndReconnect` |
| V7: frame ownership | `framebufferCopiesSurviveResizeAndFree`; held copied images after live worker teardown |
| V8: resize/update teardown | `cancelConnectDuringResizeAndDestroy`, `closeShellDuringResize`, `panelCloseStopsAllWorkers`, low-level live reconnect |
| V9: containment | `test_cache_containment`, `containedDevelopmentPathsAndTransientCredentials`, reference/bytecode/ownership audit |
| V10: scoped cleanup | Tool cleanup/symlink/unrelated-tag tests; `test_rdp_runner_isolates_cases_and_cleans_owned_containers`, failed-start preservation |
| V11: tracked pins | `git ls-files --error-unmatch Cargo.lock`, `--locked` Cargo invocations, offline verifier and hash audit |
| V12: validation before overwrite | Model invalid/cycle/version tests; `invalidSettingsPreserveFiles`, `contradictoryInheritedAccountRejectsMove`, pre-replace fault tests |
| V13: tab/cancel semantics | `tab_lifecycle_blocks_delete_reuses_profile_and_cancels_connect`, `mock_resize_input_cancel_and_reopen`, failed-session cleanup and Qt lifecycle tests |
| V14: command-local environment | `test_command_environment_preserves_home_and_caller`, Docker context/UID tests, `test_verifier_preserves_environment` |
| V15: production state | `designerShellAndNativeMenus`, CRUD/restart tests; no Designer samples in production |
| V16: autosave and recovery guards | `enterAndFocusLossAutoSave`, `failedAutoSaveRetainsValueAndBlocksTransition`, `failedSaveRetryRevertCancel`, `nativeQuitHonorsDirtyGuard` |
| V17: temporary nonpersistence | `quickSessionsDoNotPersist`, `temporaryReconnectPreservesTarget`, `quickConnectThroughShell` |
| V18: complete Save as | `saveAsConnectionIsAtomic`, `temporarySaveAsPreservesCapturedOptions`, `protectedSaveAsAndCrudSurviveRestart` |
| V19: identity/clicked targets | `clickedMovedTabTargetsCorrectSession`, `jumpToFocusesExistingTargetsWithoutConnecting`, cross-panel reuse and stale-page checks |
| V20: Disconnect/Close/fresh Reconnect | `disconnectKeepsTabCloseRemovesIt`, `profileEditsDoNotRetargetRunningSession`, live fresh UUID/worker checks |
| V21: all-panel subtree deletion | `disconnectedTabsBlockSubtreeDelete`; disconnected descendant in another panel |
| V22: transient credentials/trust | Credential Cancel, certificate Reject, masked/cleared controls, default clipboard off and session-only approval |
| V23: actual native menus/startup | `nativeMenusExportRevisedActions`, AppKit inspection/dispatch, native Quit guard and contained smoke launches |
| V24: real shell/disabled deferred actions | Saved/Quick Connect live paths; `unsupportedScreenshotAndToolsRemainDisabled` |
| V25: save boundary/warnings | Rust real directory-sync fault plus deterministic faults, `durabilityWarningBlocksPendingActionUntilAcknowledged` |
| V26: explicit saved opt-in | `vaultRememberIsExplicit`, `quickConnectNeverStoresCredentials`, bypass/wrong-master checks, live remember prompts |
| V27: bounded authenticated vault | Known-answer KDF/GCM, tamper/malformed tests, `authenticatedV3DomainRecordsAreBounded`, encrypted-byte scans |
| V28: committed binding | `rememberedCredentialsMatchCommittedTarget`, portable reopen/orphans, separate profile/login account live checks |
| V29: off-GUI cancellation | Worker/dialog tests, pending/queued cancellation, shell close during unlock, rejecting commit gate |
| V30: vault atomic outcomes | Replace/conflict/master-change/Forget faults; warning gating; failed save prevents session |
| V31: startup/idle/manual Lock | `vaultStartsLockedAndClearsOnLock`, `idleLockDoesNotDisconnectSessions`, masked inputs and live active-session Lock |
| V32: typed root/four buttons | `connectionsRootContainsAllProfiles`, `connectionsToolbarHasOnlyFourActions`, `documentRootCannotActAsProfile` |
| V33: compact grid/modes | `compactGridHasNoPersistentEditors`, mouse/keyboard editor tests, popup/spin/Enter/focus/Escape, `gridModesPreserveValueAndSelection` |
| V34: protected envelope/secrets | Document-store authentication/bounds/byte scans; `protectedConnectionsThroughShell`, no plaintext staging routes |
| V35: startup metadata gate | `protectedStartupRequiresMaster`, canceled/wrong/missing-pair/invalid-authenticated-document cases |
| V36: prepare/worker/finalize | `preparedChangesDoNotMutateOrWrite`, `pendingCandidateBlocksConflictingMutations`, detached persistence rejection, `committedDocumentReplySurvivesCancelAndClose` |
| V37: stage-before-ciphertext | `rootKeyStageFailurePreservesBothFiles`, `stagedKeyAndFailedProtectionRetainPendingRequest`, stage warning/cancellation and unprotect tests |
| V38: vault-only master change | `rootMasterChangePreservesDocumentAndRecords`, atomic master-change faults |
| V39: portability/Lock/re-unlock | Paired-copy/unprotect/reenable tests, `vaultLockClearsDocumentKeysWithoutDisconnect`, `protectedProfileSaveRequiresReunlock`, `protectedMoveRequiresUnlock` |
| V40: key retention/backup limits | Vault put/Forget/master-change/document-key tests; old-copy decrypt tests; documented lack of backup revocation |
| V41: no load/keystroke writes; one candidate | `loadingAndPreviewNeverSave`, dropdown single commit, rapid transitions, before/after replacement Lock/close tests |
| V42: committed stable-ID movement | Drag connection/folder/root, invalid/foreign drops, failed move, sort/search state, external revision, warning gating and editor-before-move tests |
| V43: compatible migration/no downgrade | `schemaOneLoadsWithoutRewrite`, `schemaTwoUpgradeIsAtomicAndPreservesIds`, `protectedSchemaUpgradeIsAtomicAndPreservesIds`, `vaultV1V2DomainUpgradePreservesDocumentKey`, `oldVaultOperationsNeverDowngradeV3` |
| V44: separate/inherited Domain safety | `domainReachesRdpWorker`, account validation, direct/inherited/moved Domain reuse rejection, retained separate login Domain |
| V45: typed immutable RDP settings | `savedOptionsReachFreeRdpSettings`, `newSettingsAutoSaveAndRestart`, edit/move capture tests, automatic resize and fixed-input live cases |

The grid driver opens cells by actual Qt mouse/keyboard input and commits with Enter/focus loss; there is no `editItem` fallback or direct user-commit signal in its oracle. Root/mode/context/navigation tests also settle active input. Fault seams hold saves or inject pre/post-replacement outcomes on real temporary files; they do not simulate physical power loss.

Movement tests obtain MIME via the production model/virtual path and send viewport drag-enter/move/drop events. A separate Qt mouse gesture reaches real `startDrag` and is canceled. This proves identity authorization and commit ordering, **not a manually driven physical OS drag-and-drop end-to-end test**. The combined live acceptance uses the same production MIME/drop route.

## Owned live RDP acceptance

Each function gets a fresh xrdp/Xorg container, random loopback-only port and ephemeral credentials. No automated case contacts user targets. Client/server output is checked for both password and master before printing/saving. Cleanup removes only containers successfully started by that runner. No stop/remove command in this work targeted the historical manual fixture; its recorded container name was absent during the audit, so continued availability is not claimed.

| Function | Evidence |
|---|---|
| `savedConnectionThroughShell` | Real Config creation/autosave, inherited account, tree double-click, credentials/certificate, embedded desktop/input/resize, reuse, Disconnect/fresh Reconnect; no session-lifecycle profile writes |
| `savedVaultCredentialsThroughShell` | Explicit remember/master creation, Lock with active session, restart/unlock/reuse without retyping password, separate profile/login account, fresh certificate, exact unchanged bytes |
| `protectedConnectionsThroughShell` | Enter/autosave fields; drag into inheriting folder with stable IDs; negotiate 800×600/16-bit; remember; confirm root protection/current master; restart with hidden profiles; unlock/reuse without credential prompt; fresh certificate; protected edit/restart; running options fixed despite saved resolution change; Lock leaves tree/session open |
| `automaticResizeHonorsProfile` | Data rows disabled/enabled: initial 800×600 with 16/24-bit negotiation, fixed frame or actual resized frame after shell resize |
| `fixedResolutionPreservesInputMapping` | Fixed 1280×720 through two panel sizes; letterbox clicks suppressed; independent server coordinates near (320,180)/(960,540) |
| `quickConnectThroughShell` | Actual button/Cancel, temporary desktop/input/reuse/reconnect/close; only explicit Save as changes profiles, without retargeting |
| `closeShellDuringResize` | Real plus mock worker in separate panels; pending resize/queued frame at close, joined workers and independent held image |
| `certificateAcceptedWithoutPromptAndReconnect` | Current test: certificate accepted without popup, Disconnect/worker exit, canceled credential reconnect retains page, fresh reconnect; follow-up result recorded under the 2026-10-15 policy update |
| `panelCloseStopsAllWorkers` | Clicked panel closure during live resize stops real/mock workers, retains General |
| `transientSecretsExcludedFromLogs` | Captured Qt messages/profile state exclude transient password |
| `liveFramesInputClipboardResizeAndReconnect` | Animated frames, off-GUI production/queued receipt, actual resized desktops, DISP, keyboard/mouse, both clipboard directions, copied ownership and fresh context |

Server input is an independent oracle, not just a widget spy. The protected case's first transport sends A/center; restart sends **B/quarter**, and the server must record B and a button event near **(200,150)**. Thus first-transport events cannot satisfy restarted input acceptance. Background readiness permits bounded RGB565 quantization for 16-bit color, then waits for new animated frames before disconnect. Fixed desktops retain aspect ratio; connected pane status is hidden and DesktopView occupies its contents. No oversized status section remains.

Domain forwarding/account binding uses synthetic settings instrumentation and local credential tests. The loopback account does not establish enterprise-domain login, Windows/NLA policies, or server enforcement of wallpaper/themes/cache/font preferences. Negotiated dimensions/color and resize/input have actual fixture evidence.

The fixture was independently upgraded earlier from Ubuntu 24.04/xrdp 0.9.24 because that server emitted bitmap updates during resize reactivation. Ubuntu 26.04 is pinned to `sha256:f144425ff09be612d6d9ad965196e9cdc23dae1f42110a8a11a3e9a8198759f7`; package inventory records resolved xrdp/xorgxrdp versions. FreeRDP validation was not weakened. One-second transport settling remains fixture pacing, not a universal reconnect guarantee. Apt repositories are not snapshot-pinned.

## Captures and audits

Opened and inspected only app-owned captures:

- `build/macos/protected-root.png`: fixed Connections root, protected Yes row, four toolbar actions, no Source/Apply or permanent editors.
- `build/macos/rdp-protected-shell.png`: owned fixture session chrome with the connected pane layout, compact property grid and changed saved resolution without retargeting. Offscreen window grabs cannot include QRhiWidget surface pixels.
- `build/macos/rdp-live.png`: decoded frame bytes saved from the worker mailbox, showing actual fixture desktop content.
- `build/macos/ui-preview/preview-native-config.png`: native preview root/grid and real Config popup, no network or storage.

These are widget captures, not host screenshots. No master/credential dialog, user desktop or unrelated application is captured; Cocoa's system menu bar is validated by AppKit rather than screenshot. Presented RHI pixels are asserted by the Cocoa `grabFramebuffer()` test instead of window grabs. Logs/captures can be overwritten by later runs.

The final audit passed: all 1,613 baseline files were unchanged, with no additions/removals in original references, root `test.ui`, `Cargo.lock` or `dependencies.lock.json`. All 5,981 generated Linux entries were owned by host UID 501. No source bytecode, unexpected untracked output or remaining automated fixture containers (running or stopped) were found. `Cargo.lock` remains tracked and `git diff --check` passed. The historical manual fixture was absent; this work did not stop/remove it. Audit output stays in `build/macos/t27-audit.json`.

## Corrected verification failures

The new 16-bit protected case initially required exact fixture RGB equality. RGB565 quantization invalidated that test oracle; bounded channel tolerance fixes readiness without weakening protocol checks. Its subsequent protection-dialog assertion did not recur in the corrected run. The final server check distinguishes first/restarted input.

Linux also exposed a production activation bug: Qt's default Enter activates items there instead of opening Config editors. `ProfileEditor` now handles Return/keypad Enter explicitly for editable values, including selection in the property column; read-only rows remain closed. A stricter keypad regression initially failed because the handler rejected `Qt::KeypadModifier`. It now ignores that modifier while retaining shortcut modifiers. The regression sends a Qt key event with the keypad modifier, without a direct-edit fallback; this is not a physical keyboard/hardware test.

The matrix initially passed several Cocoa executable names to a runner that accepts one target plus test-function names; each target now runs separately. Native preview initially lacked its asset-directory environment and skipped optional image decoding; the corrected invocation runs it. A Linux tooling run read an in-progress third-fixture unit-test update and exhausted its two-case secret mock; final complete mocks include three isolated cases. These were test/invocation defects, not specification changes.

Earlier verified save-boundary work includes a real Unix directory-sync denial plus deterministic faults, GUI/worker ledger reconciliation after Lock/Cancel, and immutable frame copies after worker destruction. Passing these checks is not a formal race proof or power-loss guarantee.

## Historical T27 limits

The limits below describe T27. Later sections above record XML, Linux-client and package evidence; their narrower remaining limits still apply.

- No XML import/export/conversion, original vault import, real SSH/VNC or VT emulation, OS keychain, Gateway/redirection/external/Multi SSH execution, split/detached views.
- No Windows build, Linux AMD64 execution, Linux-client live RDP or devcontainer editor validation. Linux full builds compile/link FreeRDP and test localhost error/cancel paths, not a Linux live desktop.
- No enterprise-domain/NLA policy matrix, universal server/reconnect compatibility or full keyboard-layout support.
- No native cross-application clipboard proof, host clipboard writes from automated Cocoa tests, Wayland/GPU benchmark, accessibility audit or pixel-identical Windows UI.
- No ThreadSanitizer/formal race proof, guaranteed zeroization, locked memory, malware resistance, rollback/key-compromise/backup revocation, or physical power-loss test. Joins can briefly pause the GUI.
- No current icon-bearing package/release/notarization/distribution signing. Per-image rights and project license remain unresolved; original resources are read-only. Historical T1–T11 package smoke checks predate this shell and do not validate current packaging.
- No cross-file atomic transaction. Protection can safely stage a vault/key before a failed document replacement. Lock leaves already opened metadata/session targets visible. Old vault copies retain access to later documents using the same key; see [vault limits](password-vault.md).

No global tool installation, shell edit, HOME change, broad Docker prune, push or PR belongs to this acceptance work.

## RDP security properties and KRdp (T67–T70, 2026-10-14)

Diagnosis (B38): a standalone probe using RdpWorker's exact FreeRDP 3.32.1 settings negotiated TLS with KDE KRdp 6.7.5 (`krdp.example:3389`), then failed capability exchange with `unexpected MCS channel id 0000` → `ERRCONNECT_CONNECT_TRANSPORT_FAILED`. Enabling only `SupportHeartbeatPdu` after channel load reached `PostConnect`; KRdp then logged off the deliberately invalid probe account (`ERRINFO_LOGOFF_BY_USER`). No real credentials were used.

| Check | Result |
|---|---|
| `rdp_security_test` (18 cases: security flag mapping/exclusion, Automatic Windows defaults, CredSSP on/off, conflicts, Server Authentication policies, certificate decisions, load-balance token, gateway never/always/detect/same/separate, message channel without autodetect, error text, XML round trip, secure defaults for old files, unsupported native values, Rust bounds, idle timer) | passed |
| `property_grid_test` `rdpSecurityAndGatewayRowsUseNativeEditors`, `configHelpMatchesReference` | passed |
| `connection_xml_test` (critical-option cases updated for V105) | 37 passed |
| Full CTest with project `TMPDIR`, `cargo test --locked`, Python unittest (103) | all passed |
| cmremote `RdpWorker` driver against KRdp, Automatic and TLS, invalid probe account | first-use certificate prompt (name mismatch, no CN), `connected`, then `ERRINFO_LOGOFF_BY_USER` from KRdp |
| User's real-credential run after first fix | reached `CONNECTION_STATE_ACTIVE`, failed ~2.5 s later: KRdp continuous RTT auto-detect request rejected because `NetworkAutoDetect` was off (`autodetect.c`); fixed by enabling it after channel load, not yet re-run with real credentials |

Not verified: full KRdp logon with real credentials, interactive certificate/credential prompts in the GUI, Windows RDP regression after enabling the message channel, any RD Gateway, load-balancing broker, or idle disconnect in a live session.

## RDP certificate policy update — 2026-10-15

The default UI choice, "Accept unverified certificates for this session", retains serialized `server_auth=warn` and now auto-accepts new, unverified, mismatched or changed certificates for that connection/session without a popup or saved trust. `always` bypasses verification without a banner. `required` still rejects unless the certificate is CA-valid or previously trusted. Server and gateway callbacks share this policy; existing FreeRDP `known_hosts` entries remain honored, with no new permanent trust writes. The worker certificate signal, dialog and wait are removed; the asynchronous credential prompt/wait remains. TLS/NLA/CredSSP negotiation is unchanged.

Earlier execution evidence, including certificate prompts/rejection, describes the policy at that time. Validation for the updated implementation:

- Runner build passed.
- Owned offscreen `certificateAcceptedWithoutPromptAndReconnect` passed (one case plus init/cleanup).
- Owned offscreen `unverifiedCertificatesFollowSessionPolicy` passed all nine data rows: new/changed/malformed certificates × warn/always/required. Checks cover unchanged stored certificate bytes and no new known_hosts files.

- Full native CTest suite passed: 30/30 tests, including `property_grid` and `rdp_security` (65.57 seconds).

The initial direct CTest invocation omitted the project-contained temporary-directory settings, failed XML setup in several suites and reached its 120-second command limit. The successful rerun used `scripts/env.py` with `TMPDIR`, `TMP` and `TEMP` set to `build/macos/test-data`, matching the project test runner. The real GNOME/CachyOS endpoint, native GUI rendering and live RD Gateway certificate handling were not tested in this update.
