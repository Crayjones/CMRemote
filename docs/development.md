# Development and usage guide

<img src="../app/assets/branding/cmremote-128.png" alt="CMRemote wireless monogram" width="128" height="128">

A native remote connection manager inspired by mRemoteNG. Qt 6 Widgets provides the C++ desktop UI; Rust owns profile metadata, inheritance and session records through cxx. C++ owns connection XML, encryption and file persistence. CMake/Ninja builds both halves. The executable and application-data identifiers remain `RemoteManager` so branding does not move existing profiles or settings.

The Designer shell has embedded **FreeRDP 3**, **SSH2 with russh and libvterm**, and an in-development **VNC client with vendored vnc-rs 0.6.0** for saved connections and temporary Quick Connect targets. Config edits save on Enter or focus loss; connections and folders can move by drag-and-drop. It includes explicit inheritance, typed protocol settings, whole-document password protection, session/panel commands and native macOS menus. SSH supports passwords, bounded OpenSSH private keys, explicit host-key approval and a configurable terminal. It does not launch or reparent an external client. VNC uses classic **unencrypted** RFB; native build, graphics, live-server and platform acceptance remain pending. Mock views are explicit test paths, not production Connect fallbacks.

## Native macOS

Requires an existing Apple compiler/macOS SDK, `/usr/bin/python3` with venv support, Perl, make, and network access. The bootstrap checks the Apple tools and stops if they are missing. It does not install Command Line Tools for you.

Required upstream UI images and their notices/provenance are retained in `vendor/mremoteng-assets/`. Builds no longer require an mRemoteNG checkout, .NET projects, or a separate clone.

The canonical macOS development workflow, from the project root, is:

```sh
./scripts/bootstrap-macos.sh --jobs 8
python3 scripts/project.py build
python3 scripts/project.py run
```

Bootstrap uses the current `dependencies.lock.json` pins and project-local SDK. `build`, `run`, `test`, and `package` all configure/build the selected current source tree before using it. A failed build prevents launch or packaging; `run` never launches an old `dist/` application. The normal target is `build/macos/app/RemoteManager.app`, not the standalone preview or `app-mock`.

Run regression tests with `python3 scripts/project.py test`. Add `--demo` to `run` only when you want example development profiles.

`run` keeps data in `.dev/macos/data/`. `--demo` seeds examples only when the development profile file is absent. Close the window to stop sessions and exit. For a brief native launch check:

```sh
python3 scripts/project.py run --smoke
# Native Cocoa widget tests, rather than the default offscreen test:
python3 scripts/env.py build/macos/app/qt_smoke
# Separate frontend build without FreeRDP linkage:
python3 scripts/project.py test --mock-only
```

Tools are downloaded or built under `.dev/macos/`; build output is under `build/macos/`. Nothing is installed in Homebrew, `/usr/local`, shell startup files, or global Rust directories. `HOME` is unchanged. Environment wrappers modify only their child commands. Downloads may be large; initial OpenSSL/FreeRDP compilation takes longer than incremental application builds.

## Linux with OrbStack / Docker

On macOS, install/start OrbStack yourself and confirm its Docker context exists. The wrapper selects `--context orbstack` for each command; it never changes the current context. On Linux, it uses the caller's current Docker context. Override command-locally with `RM_DOCKER_CONTEXT=...` if needed.

```sh
./scripts/linux.sh image
./scripts/linux.sh build
./scripts/linux.sh test
./scripts/linux.sh run --demo --smoke
./scripts/linux.sh shell
```

The image contains Linux Qt, Rust, CMake, Ninja and native libraries, separate from the macOS SDK. Container commands use your host UID/GID for the source bind mount. Linux outputs are under `build/linux/arm64/` or `build/linux/amd64/`; Cargo downloads/cache are in `.dev/linux/`. Container SDKs live in image layers at `/opt/remotemanager/.dev/linux`, not in the source bind mount. Container tests use Qt's offscreen platform and require neither XQuartz nor a host desktop environment.

ARM64 is the default on Apple Silicon. AMD64 has a separate image/builder/build directory:

```sh
./scripts/linux.sh image --arch amd64
./scripts/linux.sh test --arch amd64
./scripts/linux.sh package --arch amd64
```

AMD64 on an ARM64 host requires Docker's emulation support and is slower. The AMD64 build, full regression suite and relocated package's XCB/software-OpenGL and crypto checks passed on 2026-10-10; see [validation](validation.md). This does not establish compatibility with every Linux desktop or server. A Linux container build is **not** a native macOS build. `run` in the default container is headless, not a visible Linux desktop.

## Qt Designer shell and standalone preview

Production and the standalone preview share `app/ui/test.ui`, the curated upstream icon catalog, and the first-party `app/ui/branding.qrc` catalog. Both builds use the retained `vendor/mremoteng-assets/` subset; no full upstream checkout is needed. Edit the canonical form in Designer; production behavior is in `MainWindow.cpp`, `MainWindowMenus.cpp` and `ProfileEditor.cpp`.

The standalone preview remains non-networked and in-memory. It does not read production profiles, prompt for credentials or link Rust/FreeRDP:

```sh
python3 scripts/preview-ui.py run
python3 scripts/preview-ui.py test
python3 scripts/preview-ui.py capture
python3 scripts/preview-ui.py capture --menu tree
```

See [source mapping, Designer workflow and preview limits](ui-preview.md). Generated files stay under the platform's `build/.../ui-preview/` directory. The commands use the existing SDK and do not install Designer or .NET.

## Use the application

1. Create a folder or connection through the four-button Connections toolbar (**New connection**, **New folder**, **Expand all**, **Sort**) or the right-click menu. All profiles sit under the fixed **Connections** root.
2. Click/double-click a Config value or press Enter to edit. **Enter or leaving the cell saves it**; Escape cancels that cell. Typing alone does not write. Editing an inherited value creates a local override. Bold values are local; ordinary values inherit. The **Inheritance** view has per-field Yes/No controls; the property context menu can restore inheritance. An explicit empty string differs from inheritance. No Source labels or Apply & Save command are shown.
3. Normal selection/connect/close waits for the save. Invalid values or failed writes keep the unsaved value and the old file/model, with **Retry / Revert / Cancel** recovery. A canceled protected-save unlock blocks the pending command. Post-replacement durability warnings require acknowledgement; they do not undo the committed save.
4. Drag a connection/folder onto another folder or onto Connections to clear its parent. Drop on the upper or lower edge of a row to place it before or after that row, in the same or another folder; a line shows the insertion point and the order is saved in the file. Placing an item where it already is does nothing. A manual placement clears any A–Z/Z–A view sort. Folders have no Hostname. External/cross-window drops, cycles and connection destinations are rejected. The tree changes only after successful replacement; moving a folder preserves its subtree and UUIDs. Inherited settings recompute, while open sessions retain their captured target/options.
5. Set Protocol and Description. RDP uses separate **Username** and **Domain**, plus console/resolution/automatic-resize/color/cache/wallpaper/themes/font settings. A nonempty RDP Domain requires an unqualified Username; qualified names and UPNs remain accepted with empty Domain. SSH sends Username literally; its preserved Domain and RDP-only controls are inactive. SSH Config adds **SSH authentication** (Password/Private key) and **SSH key file** with a captured-owner chooser. Config shows common fields plus fields for the effective protocol, including inherited Protocol and folder defaults; hidden overrides remain intact if you switch back. Server negotiation can override RDP preferences; fixed-size desktops keep aspect ratio and correct pointer mapping.
6. Double-click a saved RDP/SSH2/VNC connection or choose **Connect**. Matching unlocked file passwords are reused in Password mode; otherwise the credentials prompt is session-only, with no remember control. VNC uses the reviewed saved **Password** (the VNC server password) when the server requires one, shows a minimal **Password** prompt only when the server asks and none is saved, and connects without any password when the server offers no authentication; it never sends Username/Domain. Config's masked **Password** row saves a password after association review; a custom file master is optional but recommended because the default key is public. SSH key passphrases are session-only and never taken from saved Passwords. RDP's default **Server Authentication → Accept unverified certificates for this session** (`server_auth=warn`) accepts new, unverified, mismatched or changed certificates for that connection/session without a popup or saved trust. **Don't connect** rejects unless the certificate is CA-valid or previously trusted; **Always connect** bypasses verification without a banner. The same policy applies to RDP gateways; existing FreeRDP `known_hosts` entries remain honored, with no new trust writes. SSH server keys are accepted without a prompt or saved trust. Silent acceptance does not protect against an impersonated server. Repeated Connect selects the same owning file/profile's tab across panels, including a disconnected tab. Double-clicking a folder only expands or collapses it. **Connect** from a folder's right-click menu opens every connection beneath it, one after another; a connection without a username uses the folder's, and one without a usable saved password uses the folder's saved password before prompting. **Disconnect** on a folder disconnects them all.
7. For **Quick Connect**, choose the protocol from the arrow beside the protocol button (RDP by default; VNC or SSH), then enter a hostname/IPv4 with optional `:port`, bracketed IPv6 with optional port, or bare IPv6. The default port is 3389 for RDP, 5900 for VNC and 22 for SSH. Click the protocol button or press Return. The globe button next to it lists every open connection file with its folders and connections; choosing a connection connects it, like **Connect** in the tree. Used addresses join a bounded history. This creates a temporary session, not a profile. Temporary tabs match the captured protocol/endpoint/account and never alias saved tabs. **File → Save as connection…** saves a new complete profile without its authentication secret or retargeting the temporary session. For RDP and SSH, **Save to Connections** in the Quick Connect credentials prompt opens the same dialog after connecting and also stores the typed password (SSH: Password authentication only, never key passphrases), bound to the saved endpoint and account.
8. Right-click a tab for **Disconnect**, **Reconnect**, **Close tab**, **Rename tab** or **Move to panel**. Disconnect retains the page/registration; Close stops and removes it. SSH/VNC workers retire off the GUI, with reconnect/application closure waiting asynchronously for exit. RDP retains its existing join-on-close behavior. Reconnect creates a fresh worker using committed saved settings or the retained temporary target. Saved reconnect resolves its owning file's latest settings and can reuse matching unlocked Passwords; temporary reconnect prompts again (VNC asks only if the server requires a password).
9. **View → Add Connection Panel** creates another group. **Jump To** focuses existing docks, panels or sessions without connecting or saving. Closing a panel stops its sessions. General cannot close, even after rename. Close all associated tabs, including disconnected tabs in other panels, before deleting a profile subtree.

10. Tabs and panels work like browser tabs and windows. Drag a session tab sideways to reorder it. Drag it up onto another panel's tab to move it there, or onto another window's tab strip to move it into that window. Drag it out of the strip and release elsewhere to open a new window holding it in a new panel; dragging a window's only tab moves the whole window, and dropping it on another strip merges it back so the window disappears. Panels (except General) can be dragged out into their own window and back the same way. A panel emptied by dragging its last tab away disappears. Moving never reconnects. New connections open in the window whose tabs you used last. A dragged-out window's corner menu adds a panel or shows the Connections and Config panes in that window. Closing a dragged-out window ends its sessions after confirmation. **Fullscreen (right-click a tab)** moves the session into its own window that fills the screen to every edge, with black bars only where the remote desktop's shape differs from the screen. The window it came from keeps its panels, docks and other connections, and new connections still open there. A compact bar floats over the top middle with a **pin**, **Minimize** (minimizes only the fullscreen session; restoring it returns to fullscreen), **Exit fullscreen** (puts the session back in its original tab) and **Close connection**. Closing the fullscreen window also ends its session. Unpinned, the bar slides up out of sight once the pointer leaves it and slides back when the pointer touches the top edge; pinned, it stays put. The pin choice carries over to later fullscreen sessions until the app quits.
11. Open **Options** from File, or **CMRemote → Preferences** / **Cmd+,** on macOS. Appearance controls Config grouping/property help; Tabs & Panels controls Quick Connect visibility/toolbar locking. **OK** saves and applies; **Cancel** leaves settings unchanged. Nonsecret preferences live in `preferences.json` under the application data directory (`.dev/<platform>/data` for development), independently of opened connection files.
12. The connection right-click menu matches mRemoteNG's: **Connect** (Ctrl/⌘+Shift+C), **Connect (with options)** (console session for RDP, fullscreen, without saved credentials, or into a chosen panel), **Disconnect** (ends the session and closes its tab), **External Tools**, **Transfer File (SSH)**, **Duplicate** (Ctrl/⌘+D; copies settings but not the saved password), **Rename** (F2), **Delete…** (Del), **Import**, **Export to File…**, **New Connection**, **New folder**, **Sort** and **Move up/down** (Ctrl/⌘+↑/↓).
13. **Import from File** reads mRemoteNG XML (asking for the password of a protected file) and CSV, `.rdp` files, Remote Desktop Connection Manager `.rdg` files and PuTTY Connection Manager `.dat` files into the selected folder; **Import from PuTTY** reads saved PuTTY sessions. Imported passwords are bound to their imported accounts. RDCMan passwords use Windows DPAPI and are not imported. **Export to File** writes mRemoteNG XML (passwords re-encrypted with the public default key) or CSV (plain-text passwords, as in mRemoteNG) for everything, the selected folder or the selected connection, with Username/Password/Domain/Inheritance optional.
14. **Tools → External Tools** defines programs launched for the selected connection with `%NAME%`, `%HOSTNAME%`, `%PORT%`, `%USERNAME%`, `%DOMAIN%`, `%DESCRIPTION%` and `%PROTOCOL%`; saved passwords are never substituted. Tools marked **Show on toolbar** appear on **View → External Tools Toolbar**. **View → Multi SSH Toolbar** sends one typed line to every connected SSH session. **Transfer File (SSH)** uploads one file with SCP, using the same host-key trust as SSH sessions. **Tools → Reconnect all connections** reconnects every tab, one credential prompt at a time; the Config toolbar's **Check host status** tests whether the host accepts connections on its port.

Config has four exclusive toolbar modes: **Properties**, **Inheritance**, **Default Properties**, and **Default Inheritance**. Properties is always available; Inheritance only for a connection or folder inside a folder; the two Default modes only while the connections file root is selected. Selecting something outside a mode's scope returns Config to Properties. Defaults are creation templates for new connections/folders, not changes to existing profiles or running sessions. Editing a Default Property makes it local; Default Inheritance **Yes** uses the new profile's parent or protocol baseline, while **No** stores the template value locally. **Inherit All** toggles only the available editable fields and keeps hidden protocol settings intact. No global password or key passphrase is stored; Default Password provides only a parent-inheritance policy, without granting saved-password reuse authorization.

Choose a connection icon in **Display → Icon** or the Config toolbar's icon menu. Both use the same bundled catalog and update the connection tree, saved-connections menu and owning session tabs without reconnecting. Native `Icon`/`InheritIcon` attributes round-trip; unavailable imported names remain saved and display a safe fallback, never a file loaded from that name.

File / View / Tools / Help is the menu bar; macOS uses native Preferences/About/Quit placement. Connections and Config open at startup; Notifications collects messages but starts hidden and Reset Layout hides it again. Opening a connection preserves your chosen panel visibility. View toggles/reset/toolbar lock/Full Screen (F11) and notification details/explicit Copy/Delete work. Screenshots and external/Multi SSH execution remain disabled with availability hints. No application popup intercepts RDP remote right-click input; SSH terminals have their own local Copy/Paste/appearance menu. Startup and smoke mode never connect automatically.

RDP keyboard support currently assumes a US layout, with Unicode/IME fallback. Primary/secondary/middle buttons and vertical wheel are supported. Server-supplied cursors show hand, text, resize, busy and custom shapes over the remote image; letterbox margins and disconnected views use the normal pointer. Dynamic resize depends on server DISP support; it starts after an initial desktop frame.

For RDP host ↔ remote text copy/paste, right-click the connected tab and check **Share text clipboard for this session**, or opt in at the credentials prompt. This also works when saved passwords skip the prompt. Sharing starts off on each new connection/reconnect and is never saved. Focusing the authorized desktop sends existing host text; new host copies go only to the focused session. A pending remote copy can complete after focus returns to a host application, unless another session takes the route or newer host text invalidates it. Disabling does not erase the host clipboard or recall text already transferred. Only Unicode plain text is shared, bounded to 1 MiB on the wire; files, images, HTML and the X11 selection clipboard are excluded. Worker joins can briefly pause the GUI during closure.

New production files use `confCons.xml`. Open multiple files beneath independent **Connections** roots; copied profile UUIDs do not share credentials or tabs. File format is detected from contents, not the extension. Supported native files are ConfVersion 2.6/2.7/2.8 with AES/GCM, in password-only or full-inner-XML mode; legacy CBC and unsupported crypto/version parameters are rejected. Unknown native fields are preserved, with unsupported launch options disabled. JSON remains an internal password-free metadata projection, never a password-storage channel.

Same-directory replacement is the save commit boundary. A later directory-sync failure reports **saved with a durability warning**; the owning context adopts the committed document and requires acknowledgement before continuation. Corrupt files are rejected without reset or overwrite. Opening and unlocking do not rewrite files. The **Connections** root has independent **Password protect** and **Encrypt entire file** controls. Password protect sets an optional custom master, confirmed twice; removing it confirms the public-default risk and retains saved passwords. Encrypt entire file encrypts the inner XML with the current key, without changing the master; root attributes remain public. Cancel leaves the file unchanged. XML plus its master is sufficient for backup and active-password portability; no sidecar is needed.

## File passwords and backups

Config's masked Password editor never preloads a saved secret. Enter/focus loss submits a replacement; Escape cancels; an untouched empty editor does nothing. **Clear password** and inheritance are explicit actions. Tools provides **Unlock passwords**, **Lock passwords**, and **Set or change file password** for the selected file. Saved passwords use the public `mR3m` key unless a custom master is set; this default provides obfuscation only. New custom masters accept 1–4096 single-byte/Latin-1 characters, with exact confirmation; empty, NUL-containing, whitespace-only, incompatible Unicode and reserved `mR3m` values reject. A long random password is recommended. Custom-protected files prompt automatically on open, with at most three submitted passwords and filename/path context. An unlocked file never relocks on its own; saved RDP/SSH Password Connect/Reconnect unlocks its owning file again when looking up credentials, without a master dialog for default-key files. VNC looks up its saved Password the same way, bound to its host and port. Cancel does not create a session or prompt for remote credentials. Quick Connect and **Connect with different credentials…** are session-only and never persist passwords.

Private encrypted bindings check the file, profile, password source, protocol and committed endpoint/account before reuse. Version 1 remains RDP-compatible; version 2 adds SSH Password bindings with an empty canonical Domain. Switching to private-key authentication removes Password reuse authorization without deleting its ciphertext. Default-key bindings have no attacker-resistant authenticity because their key is public. Native password ciphertext alone does not authenticate its metadata associations: imported credentials require explicit review before reuse. Original mRemoteNG resaves can drop private bindings, app-only preferences and inactive archives. Lock clears reusable keys, but leaves opened metadata and active sessions visible; it is not a screen lock. Master changes re-encrypt known XML secrets, but do not revoke old copies. Compatible PBKDF2-HMAC-SHA1 gives weaker password-guessing resistance than the retired Argon2id vault; use a long random master. Interoperability evidence is source/vector based, not a tested Windows mRemoteNG runtime.

The current application opens connection XML, not retired JSON/RMCONS/vault pairs; their old reader/conversion UI is no longer present. Keep historical source/sidecar backups together and do not overwrite them while diagnosing migration. Current active-password backups need only XML plus its master. Malformed XML never falls back to an empty document or legacy reset. Forgotten masters cannot be recovered.

## VNC desktop — implementation awaiting acceptance

The approved client baseline is **vnc-rs 0.6.0**, vendored with locally documented patches, not LibVNCClient. It implements RFB 3.3/3.7/3.8 over direct TCP with classic VncAuth or None. Like mRemoteNG, None is used whenever the server offers it, and no password is sent; otherwise VncAuth uses the saved Password or asks for one. Passwords must be **1–8 printable ASCII characters**; longer/Unicode input is rejected, never silently truncated. A rejected password is reported as such.

**Classic VNC traffic is unencrypted and has no cryptographic server-identity verification.** VncAuth's legacy challenge-response does not encrypt the desktop/input stream. Use only a trusted, isolated network or an independently secured transport; the application does not create an SSH tunnel, TLS connection or proxy for VNC.

The client negotiates ZRLE, Tight (without JPEG), TRLE, CopyRect and Raw plus server-driven desktop resize and cursor updates. It uses the existing native QRhi desktop presentation with aspect-fit scaling, keyboard/pointer input and inherited **View only** to suppress all remote input. Saved Connect, Quick Connect, disconnect/reconnect and graphical-session fullscreen use the shared shell routes. Config shows Hostname/IP and Password under Connection, Protocol and Port, and SmartSize Mode (Aspect) and View Only under Appearance; VNC has no Username/Domain. The saved Password is the VNC password, bound after review to the profile's host and port (imported passwords need review first). A password typed into the prompt is session-only. **Save as connection…** never saves that authentication.

Imported stock `VNCEncoding=EncHextile` is preserved for compatibility, but is not an instruction to advertise Hextile: the client always negotiates its fixed supported encoding set. Nondefault unsupported native security, proxy, color/compression or sizing options block Connect rather than silently invoking a different transport. There is no TLS/VeNCrypt/vendor authentication, clipboard, client-requested desktop resize, proxy, chat or file transfer. No external VNC window/client is launched or reparented.

Native application build/regression, independent live-server input/resize/cursor/authentication, graphics/fullscreen lifecycle and package acceptance have **not passed yet**. Windows source portability is pending; there is no Windows bootstrap/wrapper or native Windows evidence. Earlier RDP/SSH results do not establish VNC support on macOS, Linux or Windows. See [dependency provenance/notices](dependencies.md#embedded-vnc-dependency--2026-10-12) and [validation status](validation.md#vnc-productive-tile-drain-scheduling--2026-10-12).

## SSH terminal and appearance

SSH2 opens an embedded UTF-8 terminal with keyboard/IME input, full-screen programs, alternate screen, bounded scrollback, selection and direct-user **Copy/Paste**. Disconnected text remains copyable; inactive paste/input is disabled. There is no automatic clipboard sharing, OSC clipboard access, hyperlink launcher or remote local-command/file action.

Hold the left mouse button and drag upward or downward to select multiple lines. While holding, scroll with the wheel/trackpad or hold the pointer at the terminal's top/bottom edge to extend into retained history. Release to finish, then use **Copy** or Command+C on macOS / Ctrl+Shift+C on Linux. Scrolling after release does not change the selection; dragging never copies automatically. History selections stay anchored as output arrives, but eviction of selected text, live-grid content changes, resize, history clear or buffer switches clear unsafe selections. Full-screen alternate-buffer selection is limited to visible cells, not primary or remote tmux history.

Terminal appearance lives on the **Terminal** page of **Settings…** (the Preferences-role action); **Terminal Settings…** in SSH terminal/tab context menus opens that page directly. Choose a fixed-width font (6–48 pt), default/selection/cursor colors, all 16 ANSI colors, cursor shape/blink and bold-color behavior. The default **Classic (PuTTY-inspired)** palette reduces normal ANSI primaries from 224 to 187; **Tomorrow Night** reproduces the screenshot palette; **libvterm original** and custom edits are also available. Presets change colors only; indices 16–255 resolve through the standard xterm cube/grayscale and explicit RGB passes through unchanged. This is not PuTTY settings import or a pixel-identical appearance claim. Its preview redraws the sample whenever its grid size changes. **Settings → Appearance → Theme** picks Match System, Light (the default) or Dark; System follows later OS changes. In Dark, the Visual Studio-style toolbar/menu icons are recolored at runtime (`app/ThemeIcons.cpp`: dark glyph grays → light, #f6f6f6 outlines → dark, dark accents brightened); full-color connection icons are left unchanged.

The preview uses synthetic text only. **Apply** saves and updates open SSH terminals; **OK** saves and closes; **Cancel** discards edits since the last successful Apply. **Reset Defaults** changes the draft until applied. Font changes resize the existing PTY without reconnecting. Preferences persist separately in `terminal-appearance.json`, outside connection XML, credential bindings and `ssh-host-keys.json`. RDP rendering is unchanged.

### Terminal keyboard shortcuts

With an SSH terminal focused, physical **Control** keys go to the remote shell/application on both platforms. **Ctrl+L** requests clear/redraw; **Ctrl+C** interrupts; **Tab** completes in the remote shell without moving focus. General Ctrl, Alt/Option word-editing, navigation and function keys are supported. In remote tmux, press **Ctrl+B**, release it, then **C** to create a tmux window; N/P change windows, % splits and D detaches. These commands depend on the remote application's bindings; they do not create application windows or erase local scrollback.

| Local action | Linux / non-macOS | macOS |
|---|---|---|
| Copy / paste | Ctrl+Shift+C / Ctrl+Shift+V | Command+C / Command+V |
| Clipboard aliases | Ctrl+Insert / Shift+Insert | Shift+Insert paste, where available |
| Select retained text | Ctrl+Shift+A | Command+A |
| Scrollback page up/down | Shift+PageUp / Shift+PageDown | Shift+PageUp / Shift+PageDown |
| Scrollback start/end | Ctrl+Shift+Home / Ctrl+Shift+End | Command+Home / Command+End |
| Font size +/− | Ctrl+Plus (or Equals) / Ctrl+Minus | Command+Plus (or Equals) / Command+Minus |
| Reset session font size | Ctrl+0 | Command+0 |

Scrollback keys navigate locally only in the primary buffer; in full-screen alternate-buffer programs they go to the remote application. Font shortcuts change only the focused SSH session in 1 pt steps, bounded to 6–48 pt. They do not save preferences. Global appearance changes preserve that session's size offset; disconnect or canceled reconnect keeps it, while successful reconnect or close/reopen resets to the current global size. RDP, other tabs, editors and the appearance preview do not zoom. The terminal context menu lists local actions and shortcuts; see the [full remote-key table](ssh-api.md#keyboard-routing).

Owned independent OpenSSH/vim/tmux acceptance:

```sh
python3 scripts/test-ssh.py --platform cocoa
python3 scripts/test-ssh.py --platform xcb
```

The runner generates synthetic credentials/keys, starts only project-owned fixtures on random loopback ports, waits for the SSH banner and fixture oracle, checks reports for its secrets and removes its own resources. No user targets, `~/.ssh/known_hosts`, real host clipboard or terminal/secret captures are used. All ten cases passed Cocoa and Linux ARM64 XCB on 2026-10-11, including shell editing/completion, tmux prefix commands, session zoom, drag selection through scrollback, and indexed/truecolor equivalence outside and inside tmux with independent server-side checks. See [SSH ownership, key limits and trust decisions](ssh-api.md) and [validation](validation.md).

## Real RDP integration test

After macOS bootstrap and `./scripts/linux.sh image`:

```sh
python3 scripts/test-rdp.py
```

Each test function gets fresh owned xrdp/Xorg containers, **loopback-only** random ports and ephemeral credentials. Tests drive saved Connect and Quick Connect controls, actual credential dialogs, real desktops/input/resize/reconnect, panel closure and shell closure during in-flight updates. The low-level worker test exchanges text clipboard data without writing the host clipboard. Independent server checks verify keyboard/mouse and the low-level clipboard exchange.

`configPasswordXmlThroughShell` uses production XML storage: it enters and replaces Password through Config, accepts association review, optionally sets a custom master, then reconnects from an XML-only copy in a fresh window lifetime. Default/custom keys and password-only/full-inner-XML modes must authenticate without a transient password prompt or sidecar. Distinct A/B keypresses and mapped clicks prove each connection on the server.

`fullscreenThroughShell` exercises fixed and automatic-resize desktops through the shell-owned RDP connection bar, minimize/restore, the Exit button and captured-tab closure. It checks live frame dimensions, mapped remote input, no bar clicks forwarded remotely and byte-identical saved settings. Its app-owned captures exclude credential dialogs and active Config editors.

`multipleConnectionFilesThroughShell` opens files with copied portable UUIDs but different passwords, masters and endpoints. Two separate xrdp/PAM stores and event logs must receive disjoint input sets. Locking/editing the first file leaves both running targets unchanged; XML-only reopen restores each file independently; primary reconnect still resolves its owning file while the secondary root is selected.

Older vault/protected-shell case names now exercise the current XML/file-password flow, without restoring deleted legacy adapters. The protected case covers Config autosave, tree MIME/drop, inheritance, full metadata-hidden restart and fixed 800×600/16-bit input. This does not claim physical OS drag-and-drop, enterprise-domain login or a Windows mRemoteNG runtime test.

The runner checks output and diagnostics for every generated password/master before printing/saving, forwards secrets through scoped child environments rather than literal Docker arguments, and removes only successfully started fixtures in reverse order. Automated tests never contact user-supplied targets. See [validation results](validation.md) for exact platform results and named invariant checks.

Select individual functions when diagnosing a failure, for example:

```sh
python3 scripts/test-rdp.py savedConnectionThroughShell quickConnectThroughShell
python3 scripts/test-rdp.py --platform cocoa configPasswordXmlThroughShell multipleConnectionFilesThroughShell
python3 scripts/test-rdp.py --platform xcb configPasswordXmlThroughShell multipleConnectionFilesThroughShell
```

The cursor/clipboard shell checks use a separate target and an injected in-memory host clipboard:

```sh
python3 scripts/test-rdp.py --target rdp_session_integration --platform cocoa remoteCursorsThroughShell bidirectionalTextClipboardThroughShell
# Use --platform xcb or wayland for the owned Linux native client.
```

The remote fixture verifies cursor-region input and exact Unicode host→remote text independently. These tests never access the real host clipboard. See validation for actual passes and blockers; a runner option alone does not prove platform support.

`offscreen` is the default CPU/transport path. Cocoa uses the native macOS application launcher. Linux `xcb`/`wayland` clients run in an owned development container with an owned display, sharing only the fixture's network namespace; no XQuartz or host desktop is needed. A supported runner option is not a claim that every platform was tested.

Logs, package inventory and app-owned widget captures stay in `build/macos/`, including `rdp-integration.log`, `rdp-fixture-packages.txt` and `rdp-shell.png`. The test-server image uses a separately pinned Ubuntu 26.04 base with xrdp 0.10.1; the development image remains Ubuntu 24.04 and all SDK pins are unchanged. The older 24.04 xrdp server sent bitmap updates before resize reactivation completed. Updating only the owned fixture fixed that acceptance failure without relaxing FreeRDP validation. Tests retain a one-second settling delay between transports because xrdp's Xorg desktop survives disconnect. This does not establish immediate-reconnect compatibility with every server, Windows/NLA policy behavior or native clipboard interoperability.

## Branding assets

`app/assets/branding/source.png` is the byte-identical supplied logo. PNG sizes, `cmremote.icns`, `cmremote.ico`, and `favicon.ico` preserve its composition and are checked in so every platform builds without an image-processing dependency.

```sh
python3 scripts/branding.py verify
# Regenerate on macOS with the original source, sips, and iconutil:
python3 scripts/branding.py generate
```

The logo is used by the application, main/detached windows, About dialogs, preview, macOS bundle, and Linux desktop icons. Functional icons and saved connection-icon names are unchanged. There is no web frontend; `favicon.ico` and the 16/32-pixel PNGs are reusable browser assets, not a new web app.

## Package

CMRemote is licensed **GPL-2.0-or-later** ([LICENSE](../LICENSE)). Packages embed mRemoteNG icons, redistributed under that project's GPL v2 distribution with its notices; per-image provenance is recorded in `vendor/mremoteng-assets/`. Published installers are built by GitHub Actions; see [Releases and updates](#releases-and-updates).

Use the same current bootstrap/build/run workflow for the production application and its package, not a copied historical bundle. `package` also rebuilds before staging. These wrappers compile the application as **Debug** unless you add `--release`, which uses an optimized build in `build/<platform>/app-release` and skips test executables; published installers use `--release`. Neither is signed with a distribution certificate. Choose an unused artifact name: existing packages are never overwritten.

```sh
# macOS: the same .dev SDK and production application used during development.
./scripts/bootstrap-macos.sh --jobs 8
python3 scripts/project.py build
python3 scripts/project.py run
python3 scripts/project.py package --artifact-name CMRemote-branding

# Linux: image bootstraps its contained SDK; repeat for --arch amd64.
./scripts/linux.sh image --arch arm64
./scripts/linux.sh build --arch arm64
./scripts/linux.sh run --arch arm64 --smoke
./scripts/linux.sh package --arch arm64 --artifact-name CMRemote-branding
```

### Release installers

`scripts/release.py` wraps packaged trees into named installers under `dist/<platform>/release/`. The [release workflow](#releases-and-updates) runs these same steps on GitHub; use them locally only to test a change to packaging. None of these are signed or notarized.

```sh
# macOS arm64 (native) and x64 (cross-built; Qt is universal, the native SDK is per-CPU in .dev/macos/amd64).
./scripts/bootstrap-macos.sh --jobs 8
python3 scripts/project.py package --release --artifact-name CMRemote-0.1.0-macos-arm64
python3 scripts/release.py macos --arch arm64 --bundle dist/macos/CMRemote-0.1.0-macos-arm64.app
./scripts/bootstrap-macos.sh --jobs 8 --arch amd64
RM_TARGET_ARCH=amd64 python3 scripts/project.py package --release --artifact-name CMRemote-0.1.0-macos-x64
python3 scripts/release.py macos --arch x64 --bundle dist/macos/CMRemote-0.1.0-macos-x64.app

# Linux x86_64 AppImage and Flatpak (org.kde.Platform runtime from Flathub).
./scripts/linux.sh image --arch amd64
./scripts/linux.sh package --arch amd64 --release --artifact-name CMRemote-0.1.0-linux-x86_64
python3 scripts/docker.py release-image --arch amd64
python3 scripts/docker.py release --arch amd64 --package dist/linux/amd64/CMRemote-0.1.0-linux-x86_64

# Windows x64 setup.exe, cross-compiled with llvm-mingw in a Linux container (SDK in .dev/windows).
python3 scripts/docker.py windows-image
python3 scripts/docker.py windows python3 scripts/bootstrap-windows.py --jobs 8
python3 scripts/docker.py windows python3 scripts/project.py package --release --artifact-name CMRemote-0.1.0-windows-x64
python3 scripts/docker.py windows python3 scripts/release.py windows --package dist/windows/CMRemote-0.1.0-windows-x64
```

| Installer | Checked here |
|---|---|
| `CMRemote-0.1.0-macos-arm64.dmg` | Mounts; strict signature check; packaged app `--smoke` |
| `CMRemote-0.1.0-macos-x64.dmg` | Mounts; every Mach-O has x86_64 code; not launched (no Rosetta on the build host) |
| `CMRemote-0.1.0-linux-x86_64.appimage` | `--smoke` in the x86_64 Ubuntu 24.04 container |
| `CMRemote-0.1.0-linux-x86_64.flatpak` | Installs from the bundle with its Flathub runtime; every app/plugin library resolves against org.kde.Platform 6.11, and `--smoke` passes through the runtime loader. `flatpak run` itself was not exercised because bubblewrap's seccomp filter cannot load under x86 emulation |
| `CMRemote-0.1.0-windows-x64-setup.exe` | Installer payload matches the packaged tree file-for-file; every DLL is shipped or a Windows system DLL. **Not run on Windows**, and Wine under emulation was too slow to finish |

The Windows installer writes to `Program Files\CMRemote`, adds a Start menu shortcut and an Add/Remove Programs entry, and its uninstaller leaves user settings and connection files in place. The executable keeps its `RemoteManager` name and settings location on every platform.

The named macOS package is `dist/macos/CMRemote-branding.app` / `CMRemote-branding-macos.zip`; Linux uses `dist/linux/<arch>/CMRemote-branding/` / `CMRemote-branding-linux-<arch>.tar.gz`. Each contains the branding manifest, favicon, retained UI notices/provenance, and `build-info.json` recording its **source build executable** SHA-256, SDK pins, dependency-lock hash, and supplied-logo hash. macOS deployment/signing changes the packaged executable, so its final checksum is not the recorded pre-deployment source checksum.

Without `--artifact-name`, historical default filenames remain:

- macOS: `dist/macos/RemoteManager.app` and `RemoteManager-macos.zip`. Qt frameworks/plugins and native dylibs are deployed and paths checked. Ad-hoc signed for local development; **not notarized or distribution-signed**. Application binary is native ARM64 on this host, not universal.
- Linux: `dist/linux/arm64/RemoteManager/` and `RemoteManager-linux-arm64.tar.gz` (or `amd64`). Bundled Qt/FreeRDP/OpenSSL/zlib/ICU libraries, XCB GLX/EGL integration plugins and the matching OpenSSL `legacy.so` provider have relative rpaths. Run the **top-level `RemoteManager` launcher**, which selects bundled Qt plugins and OpenSSL modules. Do not use a system OpenSSL provider with the bundled libraries. Requires Ubuntu 24.04-compatible glibc and the system graphics/X11/Wayland libraries listed in `Dockerfile`; this is not an AppImage. An optional SVG-dependent Wayland decoration plugin is omitted because QtSvg is outside the minimal SDK.

Linux packages include `share/applications/org.remotemanager.RemoteManager.desktop` and hicolor app icons. The desktop file is a system-install template. For an optional per-user menu entry pointing to the portable launcher and bundled icon, run `python3 install-desktop.py` from the extracted package. Nothing is installed automatically; rerun the installer after relocating the package. Ordinary launches remain `./RemoteManager`.

A physical GPU is not required when Mesa supplies software OpenGL (llvmpipe). On Debian/Ubuntu, the runtime packages include `libgl1-mesa-dri` and `libglx-mesa0`. Launch from the VM's desktop terminal, not a plain SSH session without a display. The app still uses QRhiWidget; no QPainter fallback or global rendering override is needed. Wayland interaction for the repaired package remains unverified. RDP certificate policy is separate: **Accept unverified certificates for this session** accepts unverified or changed certificates for the session without a popup or saved trust. Choose **Server Authentication → Don't connect** to require a CA-valid or previously trusted certificate. This policy does not change TLS/NLA/CredSSP negotiation; do not disable NLA to address a certificate problem.

To preserve an older archive, choose an unused artifact name:

```sh
./scripts/linux.sh package --arch amd64 --artifact-name RemoteManager-runtime-fix
```

Copy `dist/linux/amd64/RemoteManager-runtime-fix-linux-amd64.tar.gz` to the VM, then extract it separately and launch:

```sh
tar -xzf RemoteManager-runtime-fix-linux-amd64.tar.gz
./RemoteManager-runtime-fix/RemoteManager
```

After building tests, check the relocated archive's crypto, production smoke launch and real XCB/RHI presentation with software OpenGL:

```sh
./scripts/linux.sh shell --arch amd64 -c \
  'python3 scripts/test-package.py dist/linux/amd64/RemoteManager-runtime-fix-linux-amd64.tar.gz --rhi-probe build/linux/amd64/app/qt_smoke --qt-test-library /opt/remotemanager/.dev/linux/qt/lib/libQt6Test.so.6'
```

The verifier uses disposable project-contained data, removes SDK/backend environment overrides, audits runtime paths, and requires all four pixel/presentation checks to pass without skips. Its QtTest executable/library are test-only and never enter the shipping archive. It contacts no remote server. The stronger SDK-hidden, network-disabled verification command is recorded in [validation](validation.md).

The executable without `--dev-root` keeps `confCons.xml` and its other data in `~/.config/CMRemote` on macOS and Linux (`$XDG_CONFIG_HOME/CMRemote` when set) and `%APPDATA%\CMRemote` on Windows. On first launch it copies existing data from the older `~/Library/Application Support/RemoteManager/RemoteManager` (or `~/.local/share/RemoteManager/RemoteManager`) location; the old copy is left in place. `python3 scripts/project.py run` passes `--dev-root`, so development runs use `.dev/<platform>/data` instead. To keep packaged-app validation inside the project:

```sh
dist/macos/RemoteManager.app/Contents/MacOS/RemoteManager --dev-root "$PWD" --smoke
# Within the Linux development container:
dist/linux/arm64/RemoteManager/RemoteManager --dev-root /workspace --smoke
```

Locally built packages are development artifacts; published releases come from GitHub Actions. See [dependencies and licenses](dependencies.md) for third-party notices and obligations.

## Releases and updates

Releases are built and published by GitHub Actions; nothing needs to be built locally.

1. On GitHub, open **Actions → Release → Run workflow**, enter the new version (for example `0.2.0`) and run it from `main`.
2. The workflow sets that version in `CMakeLists.txt` (the single source the app, installers and About dialog read), commits it, and tags `v0.2.0`.
3. It builds the macOS arm64 and x64 disk images, the Windows installer and portable zip, and the Linux AppImage and Flatpak from that tag as optimized Release builds, then publishes them with `SHA256SUMS.txt` on the [Releases page](https://github.com/Crayjones/CMRemote/releases). Release notes list the merged changes automatically.

`scripts/set-version.py` prints the current version, or sets a new one (`python3 scripts/set-version.py 0.2.0`); it refuses malformed or older versions. The CI workflow runs the full test suite in the Linux container on every push to `main` and every pull request.

RDP H.264 video uses Cisco's OpenH264, which CMRemote never ships: on first launch the app downloads Cisco's prebuilt library onto that device, checks its SHA-256 and loads it. **Options → Remote Desktop** turns it off or on; without it, sessions use the other RDP graphics formats. See [dependencies](dependencies.md).

In the app, **About CMRemote** shows the version and the project link. **Check for Updates…** (in the CMRemote menu on macOS, the Help menu on Windows and Linux) asks GitHub for the latest release.

### Automatic updates

**Settings → Updates → Automatically check for and download new versions** (on by default) checks GitHub at startup and once a day; nothing is scheduled until the stored setting has loaded. **Check for Updates…** uses the same check and downloads even when the setting is off.

- **Who updates itself:** the macOS app in a writable folder (not run from the disk image or a translocated copy) and the Linux AppImage. A copy qualifies only when its `build-info.json` says `"update_channel": "github"` with the running version and architecture, which only the Release workflow writes (`project.py package --update-channel github`). Development builds, `--dev-root`, `--smoke` and test windows never update. Windows setup, portable zip, Flatpak and the Linux tarball get a link to the download instead; Windows self-installation stays off until it has been accepted on real Windows with UAC.
- **Download:** the matching asset (`CMRemote-<version>-macos-<arch>.dmg` or `-linux-x86_64.appimage`) goes to `<data>/updates` (private, one owning CMRemote instance). Every chunk is checked against the published size and written in full; the SHA-256 must match GitHub's asset digest, or the single `SHA256SUMS.txt` entry for older releases. The verified package is recorded in `pending.json` and survives restarts without another network check (it is rehashed first). The checksum proves the file is what the GitHub release published; the packages are still unsigned and not notarized.
- **Your choice:** nothing installs until you choose. When an update is ready, CMRemote asks once per run: **Restart & Update** installs now, **Install When I Quit** installs the next time you quit, **Not Now** (the default, also Escape or closing the dialog) asks again at the next start, and **Skip This Version** deletes it and waits for a newer release. **Check for Updates…** shows the question again, so you can change your mind, and offers a skipped version again. Nobody is ever required to update.
- **Install:** once you have chosen, quitting first runs the usual save and session shutdown checks, then rehashes the package and starts a small helper that acknowledges and waits. Only the final close tells the helper to proceed; a canceled quit or a crash leaves it inert. After CMRemote exits, the helper checks the package again, validates it (macOS: `CMRemote.app` with the same bundle identifier, the new version and this Mac's architecture; AppImage: the AppImage header), copies the new version beside the old one, swaps it in, restores the old version if anything fails, and relaunches only after **Restart & Update**. If preparation fails, **Restart & Update** leaves CMRemote open; a normal quit offers **Quit Without Updating**.
- **Results:** the next start reports the helper's outcome in Notifications. A version whose installation failed is not offered again automatically; **Check for Updates…** offers it again.

Merging code does not update anyone: an update exists only once the Release workflow publishes a newer version with its assets and `SHA256SUMS.txt`. Copies older than the first updater-enabled release must be updated by hand once.

Tests use local fixture releases and disposable targets only: `auto_update` (CTest) covers checks, checksums, corrupt/oversized/incomplete downloads, cancellation, recovery, duplicate instances, manual updates with the setting off, helper commit/cancel/failure, the prompt's choices (install now/at quit, Not Now, Skip This Version), a single Check for Updates answer and the window's quit handoff. `python3 scripts/test-auto-update.py` builds real disk images with `scripts/release.py` around synthetic bundles and exercises the macOS helper (install, relaunch, cancel, wrong identifier/version/architecture/checksum, read-only folder, concurrent install) plus the AppImage helper; add `--linux-image <image>` to run the AppImage helper under the container's `dash`. Not yet validated: Windows/UAC, a real x86_64 AppImage runtime, and an update between two published releases.

The installers are not signed with an Apple Developer ID or a Windows code-signing certificate. macOS asks you to allow the app under **System Settings → Privacy & Security → Open Anyway** the first time; Windows SmartScreen may ask you to choose **More info → Run anyway**.

## Cleanup

Cleanup is dry-run by default. Review the printed targets, then add `--yes`:

```sh
python3 scripts/cleanup.py --platform macos --scope build
python3 scripts/cleanup.py --platform linux --scope dist
python3 scripts/cleanup.py --platform both --scope tools
python3 scripts/cleanup.py --platform both --scope all
./scripts/linux.sh clean
```

`build` removes build/test output; `dist` removes packages; `tools` removes project SDKs, toolchains, downloads and caches. `all` combines those three and **preserves development profiles**. `--scope data` or `--include-data` explicitly removes development profiles. Normal OS application-data is never touched. Symlink escapes are rejected; source files are preserved.

`./scripts/linux.sh clean --yes` removes only this checkout's labeled containers/image tags, named Buildx builders/caches, and Compose networks. It includes the RDP fixture image. It does not run Docker system prune or remove shared base images, the OrbStack VM, unrelated resources, or bind-mounted source/data. No destructive tool/cache cleanup was performed as part of milestone validation.

## Validation and limits

See [validation](validation.md) for commands, evidence and limitations. Native SSH acceptance passed macOS ARM64 Cocoa and Linux ARM64 XCB; Windows, AMD64 and Wayland SSH remain untested. Earlier AMD64 RDP/package evidence does not establish SSH support. Relocated package probes test startup/linkage/notices, not archive-to-server SSH connections. Headless tests do not establish native clipboard interoperability, GPU performance, focus behavior on every desktop, or Wayland correctness. VNC networking is under implementation but has no native/live/platform acceptance yet. Windows source portability/builds, OS keychain integration and split/detached views remain future work; renderer/performance work is tracked separately. There is no Windows wrapper or native Windows evidence.

- [Architecture and ownership](architecture.md)
- [Pinned dependencies and licenses](dependencies.md)
- [Verified FreeRDP API contract](freerdp-api.md)
- [SSH transport, trust and terminal appearance](ssh-api.md)
- [Specification](../SPEC.md)
