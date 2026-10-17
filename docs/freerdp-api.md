# FreeRDP 3.32.1 integration contract

All declarations below were read from the [3.32.1 source](https://github.com/FreeRDP/FreeRDP/tree/3.32.1). `dependencies.lock.json` records the release archive's SHA-256; `scripts/verify-dependencies.py` checks it and the symbols. This is API verification, not evidence that a live server accepted every operation.

## Context, event loop and channels

[`include/freerdp/freerdp.h`](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/include/freerdp/freerdp.h):

```c
freerdp* freerdp_new(void);
BOOL freerdp_context_new(freerdp* instance);
BOOL freerdp_connect(freerdp* instance);
DWORD freerdp_get_event_handles(rdpContext* context, HANDLE* events, DWORD count);
BOOL freerdp_check_event_handles(rdpContext* context);
BOOL freerdp_abort_connect_context(rdpContext* context);
BOOL freerdp_shall_disconnect_context(const rdpContext* context);
BOOL freerdp_disconnect(freerdp* instance);
BOOL freerdp_disconnect_before_reconnect_context(rdpContext* context);
BOOL freerdp_reconnect(freerdp* instance);
void freerdp_context_free(freerdp* instance);
void freerdp_free(freerdp* instance);
```

Set `ContextSize` before context allocation; an extended context starts with `rdpContext`. `PreConnect`, `PostConnect`, `PostDisconnect`, `LoadChannels`, `VerifyCertificateEx`, and `VerifyChangedCertificateEx` are callbacks. Use the typed `freerdp_settings_set_*` accessors from `settings.h`, not assumed struct offsets.

Register `freerdp_channels_load_static_addin_entry` through `freerdp_register_addin_provider`; set `LoadChannels = freerdp_client_load_channels` ([client/common/client.c](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/client/common/client.c)). Subscribe to `ChannelConnected`/`ChannelDisconnected` on `pubSub`. The event's `pInterface` supplies channel context, whose lifetime ends on disconnect. Never retain that pointer in GUI code.

`WaitForMultipleObjects`/`freerdp_check_event_handles` drive decoding on the worker. An owned manual-reset command event reserves a wait slot and wakes input/resize/authorized clipboard/stop without polling. Queue swap and event reset share the enqueue mutex; bounded drains preserve network fairness. All protocol calls except the documented abort signal remain on that worker. Protect abort against concurrent context destruction and close the command event only after worker join.

## Input

[`include/freerdp/input.h`](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/include/freerdp/input.h):

```c
BOOL freerdp_input_send_keyboard_event_ex(rdpInput* input, BOOL down, BOOL repeat, UINT32 rdp_scancode);
BOOL freerdp_input_send_unicode_keyboard_event(rdpInput* input, UINT16 flags, UINT16 code);
BOOL freerdp_input_send_mouse_event(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
BOOL freerdp_input_send_extended_mouse_event(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
```

Physical key presses need RDP scancodes and matching releases, including extended keys. Unicode events serve committed text, not shortcuts. Release held keys when focus leaves; modifier/layout/IME behavior needs native tests on each platform. RDP coordinates refer to remote desktop pixels, not device-pixel-ratio-scaled widget coordinates. Wheels encode signed rotation in pointer flags.

## Text clipboard

[`include/freerdp/client/cliprdr.h`](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/include/freerdp/client/cliprdr.h) defines `CliprdrClientContext`; all message types are in [`channels/cliprdr.h`](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/include/freerdp/channels/cliprdr.h).

The channel negotiates even when session sharing is off, so the tab action can enable it without reconnecting. `FreeRDP_ClipboardFeatureMask` enables only `CLIPRDR_FLAG_LOCAL_TO_REMOTE | CLIPRDR_FLAG_REMOTE_TO_LOCAL`; file directions remain disabled.

- On `MonitorReady`, send capabilities with `CB_USE_LONG_FORMAT_NAMES` only. Advertise `CF_UNICODETEXT` only after an authorized local snapshot exists; no initial empty-text offer.
- Acknowledge `ServerFormatList`; request Unicode text only for the authorized focused route. Disabled sessions acknowledge without requesting data.
- Serialize requests: replies have no request ID. Retain the outstanding origin across disable/re-enable, discard obsolete replies, then request the newest pending offer. Failed/malformed responses release the request. An unanswered request has a 15-second monotonic deadline; expiration closes the session rather than misattributing a late reply to a replacement request. This deadline also applies to an outstanding request retained after sharing is disabled.
- Validate `ServerFormatDataResponse` success, even size, final terminator, Unicode surrogates and the 1 MiB bound before delivering owned text. Reject embedded NUL. LF/CRLF/bare CR normalize to wire CRLF and host LF; the bound applies after expansion.
- Answer `ServerFormatDataRequest` only from a ready, authorized, focused worker snapshot; otherwise return `CB_RESPONSE_FAIL`. No worker calls Qt clipboard APIs.
- One latest owned delivery and one pending GUI notification carry permission/route/host revisions. GUI routing rejects stale permission, replacement routes and newer host text. Source/equality tracking suppresses immediate and delayed echo. Files/images/HTML and selection clipboard are excluded.

## Remote cursors

[`include/freerdp/graphics.h`](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/include/freerdp/graphics.h) defines the `rdpPointer` prototype. Register client `New`, `Free`, `Set`, `SetNull`, `SetDefault` and `SetPosition` callbacks after GDI initialization, leaving FreeRDP's pointer-cache/update callbacks intact.

`Pointer_Alloc` uses zeroed C storage, so the extension holds an explicitly allocated `QImage*`, not an unconstructed embedded C++ object. The adapter deletes only that image; FreeRDP owns the base and masks. Owned published image values survive pointer replacement/free and context teardown. `freerdp_image_copy_from_pointer_data` converts validated 1/8/16/24/32-bit masks, including palette/alpha/padding/orientation. Raw dimensions are limited to 384×384; hotspots must lie inside the image. Upstream approximates legacy inverted pixels with a checkerboard, rather than exact background inversion.

A bounded latest-value mailbox hands images to the current GUI connection generation. Only the GUI constructs `QPixmap`/`QCursor`, scales size/hotspot using desktop presentation and DPR, and caps physical cursor dimensions at 1024. Hidden/default, letterbox/outside-view restoration and disconnect/reconnect reset are explicit. Server `SetPosition` is acknowledged without moving the global pointer.

## Resize

[`include/freerdp/client/disp.h`](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/include/freerdp/client/disp.h):

```c
typedef UINT (*pcDispCaps)(DispClientContext*, UINT32 MaxNumMonitors,
                         UINT32 MaxMonitorAreaFactorA, UINT32 MaxMonitorAreaFactorB);
typedef UINT (*pcDispSendMonitorLayout)(DispClientContext*, UINT32 NumMonitors,
                                      DISPLAY_CONTROL_MONITOR_LAYOUT* Monitors);
```

Wait for `DisplayControlCaps` before `SendMonitorLayout`. Single-primary-monitor dimensions obey channel limits (width 200–8192, even width; height 200–8192) and server area limits. Debounce resize; if channel unavailable, keep current remote resolution and scale the view. The isolated T36 `DesktopResize` calls `resizeRdpLegacyDesktop` using negotiated settings, not requested dimensions. It validates context/GDI/codecs, positive dimensions at most 8192 per axis and at most 16 Mi pixels, then checks `freerdp_client_codecs_reset(codecs, FREERDP_CODEC_PLANAR, width, height)` before `gdi_resize`. Same-size resize is a no-op. Pinned `gdi_resize` flushes EndPaint against the old primary before replacement; reset/resize failure returns FALSE through ordinary worker teardown. Actual allocation failure need not retain a usable primary, so decoding must not continue.

## Frames, cancellation and reconnect

[`include/freerdp/gdi/gdi.h`](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/include/freerdp/gdi/gdi.h) exposes `gdi_init`, `gdi_resize`, `gdi_free`, `primary_buffer`, `width`, `height`, `stride`. [`client/Sample/tf_freerdp.c`](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/client/Sample/tf_freerdp.c) demonstrates `BeginPaint`, `EndPaint`, `DesktopResize` callbacks.

The software GDI applies *every* incremental update in order. Only then may presentation be coalesced. `DesktopUpdateAccumulator` copies dirty tiles into owned RGB32 patches inside the synchronized producer callback; no queued image or Rust slice aliases GDI memory. Replacing overlapping tiles never discards disjoint damage. Initialization, geometry changes and bounded-backlog recovery use a full snapshot. GFX mapped-surface damage is unioned across the completed flush before capture. The GUI applies batches to its retained desktop and uses owned QRhi partial uploads; resource recreation requires a full upload. Frame publication carries session identity and geometry/revision stamps; old-session updates never reach a reopened tab. `takeFrame()` is a compatibility adapter, not the production SessionPane path.

Shutdown: mark stopping → signal `freerdp_abort_connect_context` under a lifetime lock → worker exits its event loop → disconnect → clear channel pointers → free GDI/context/instance on worker → join worker before destroying its UI owner. Cancel does not free context from GUI thread. Pending immutable frames remain valid after teardown. Reconnect uses a new context and session ID, avoiding stale channels and frames; it does not claim transparent automatic recovery.

Certificate policy follows the profile's Server Authentication (I.rdpsecurity, V100). `IgnoreCertificate` is set only for "Always connect", which bypasses verification without a warning banner. Otherwise FreeRDP accepts CA-valid certificates and those already in its known_hosts store under the app's `freerdp` config directory, and calls `VerifyCertificateEx`/`VerifyChangedCertificateEx` for the rest. "Accept unverified certificates for this session" (`server_auth=warn`, default) returns 2 for new, unverified, mismatched or changed certificates, accepting them for that connection/session without a popup or saved trust. "Don't connect" (`required`) returns 0 for certificates that are not CA-valid or previously trusted, without prompting. No callback returns 1 or writes new permanent trust; existing known_hosts entries remain honored. Server and gateway certificates use the same callback policy.

Security Protocol maps to exclusive `RdpSecurity`/`TlsSecurity`/`NlaSecurity`/`ExtSecurity` flags; Automatic keeps FreeRDP's negotiation and Use CredSSP = No clears NLA/Extended NLA. There is no retry with a weaker mode. `AuthenticateEx` answers FreeRDP credential requests (missing server credentials, separate gateway credentials) with a session-only prompt. `SupportHeartbeatPdu` and `NetworkAutoDetect` are enabled after `freerdp_client_load_channels`: set earlier, the client loader would require the unbuilt `rdpdr` channel. KDE KRdp sends message-channel auto-detect PDUs during connection and continuously once active; without the channel capability exchange fails, and without `NetworkAutoDetect` FreeRDP fails the session on the first RTT request (B38). Failures report `freerdp_get_last_error_name` plus a hint. Passwords stay in transient memory, never profile JSON or diagnostics. FreeRDP trust/config directories must follow development-mode containment.

Audio: the profile's Sounds value sets `AudioPlayback` ("Bring to this computer") or `RemoteConsoleAudio` ("Leave at remote computer"). With `AudioPlayback`, `freerdp_client_load_channels` adds `rdpsnd` (static and dynamic) and registers `rdpdr`; the bundled macOS FreeRDP builds `rdpsnd` with the CoreAudio (`mac`) subsystem and `rdpdr` without drive/printer/serial/parallel/smartcard channels, so no client devices are exposed. Linux builds keep audio channels off for now.
