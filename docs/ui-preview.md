# Source-driven mRemoteNG UI preview

`app/ui/test.ui` is the approved Qt Designer shell shared by production RemoteManager and the standalone preview. It follows the supplied WinForms shell at the [pinned mRemoteNG revision][upstream-ui]. Only the curated images and notices in `vendor/mremoteng-assets/` are build inputs; the full upstream checkout is not required. Production uses Rust-backed profiles and embedded FreeRDP; the preview remains separate, non-networked and in-memory. Root `test.ui` is an older prototype and remains unchanged.

Production clears Designer samples before asynchronously loading each validated connection file into its own detached Rust Core and Connections root. Controllers supply autosave, file passwords and lifecycle commands. Preview classes supply local samples/placeholders only. Form changes affect both builds, so test both. The shared Config is a compact two-column grid with categorized/alphabetical and Properties/Inheritance modes, no Source labels, permanent editors or Apply & Save. Preview values/root protection remain display-only. Its Password row has no secret handler or editable input and cannot accept or store passwords. File New/Open/Save/Save as/Close/conversion, file-password and different-credential commands are unavailable. Preview has no storage service, production-file access, Rust, FreeRDP or OpenSSL dependency. Explicit form dependencies avoid stale controller layouts after Designer changes.

Both builds share File / View / Tools / Help, the approved View order, F11, and exactly four Connections toolbar actions: New connection, New folder, Expand all, Sort. Jump To focuses existing docks/panels/pages by live widget identity without creating a session. Screenshots/external/Multi SSH execution remain disabled. Production drag uses a validated stable-ID persistence route; preview drag remains disabled. A separate disabled PuTTY sample root remains in preview, not in production.

## Build and open

Use the existing project SDK. These commands do not install Designer, .NET, Windows fonts or additional Qt modules.

```sh
# Native macOS, from the project root:
python3 scripts/preview-ui.py run
python3 scripts/preview-ui.py test
python3 scripts/preview-ui.py capture
python3 scripts/preview-ui.py capture --menu tree
python3 scripts/preview-ui.py capture --menu session
python3 scripts/preview-ui.py capture --offscreen

# Within the existing Linux ARM64 development image:
./scripts/linux.sh shell -c 'python3 scripts/preview-ui.py test'
./scripts/linux.sh shell -c 'python3 scripts/preview-ui.py capture --offscreen --menu tree'
```

`build` only configures/compiles. `run --smoke` exits after 500 ms. `--menu` accepts `tree`, `session`, `group`, `config`, `notifications` or `external-tools`; it sends a Qt context-menu event to that surface in the running preview. `run --menu session` leaves the window open for interaction.

`capture` saves widget contents and exits. With `--menu`, it combines the window and actual popup-widget captures into one image, expanding the canvas if the popup extends beyond the window. It does not capture OS chrome, other applications or the host desktop. Output is restricted to PNG files directly inside this preview's build directory; symlink destinations are rejected.

The standalone `app/preview/CMakeLists.txt` links only Qt Widgets/Test. It does not configure Cargo or FreeRDP. The wrapper uses `scripts/env.py`; `HOME`, shell startup files and global installations are untouched. Generated headers, resource compilation, temporary test data, staged images, provenance and captures go to:

- macOS: `build/macos/ui-preview/`
- Linux: `build/linux/arm64/ui-preview/` or `build/linux/amd64/ui-preview/`

The wrapper validates and stages `vendor/mremoteng-assets/` into `assets/mremoteng-assets/` beneath that build directory. `RM_PREVIEW_ASSET_DIR` points to its `reference/` subdirectory, containing exactly `reconnect.png` and `external-tools.png`. Old generated flat `assets/main.ico` fixtures are not read or deleted. Neither the full `mRemoteNG/` checkout nor `app/ui/old-form-ui/` is a build/runtime dependency. This local prototype is not a redistributable bundle.

## Qt Designer

Open `app/ui/test.ui`, not root `test.ui`. Keep its adjacent resource catalogs and `vendor/mremoteng-assets/` at their current relative paths. `preview-icons.qrc` points directly to curated standalone PNG/ICO files, so Designer can show those static icons without the full upstream checkout or generated source-tree copies. The application-owned logo uses a separate resource catalog.

The `.ui` defines the menus, named actions and their icons, quick-connect controls, dock contents, property/help splitter, Notifications splitter, initial tree/tab icons and light stylesheet. `PreviewWindow.cpp` arranges docks and supplies dynamic data. `PreviewMenus.cpp` attaches the six context menus and local handlers. Runtime code also supplies the search-field icon, dynamic tabs, protocol choices and the two safely extracted session-menu images. Designer alone does not reproduce runtime docking or popup state rules.

The contained minimal qtbase SDK includes `uic`, but no Designer executable. No reference font is loaded or installed.

## Source mapping

Paths in this table start at [`mRemoteNG/UI/` in the pinned upstream revision][upstream-ui]. They are research citations, not local build inputs. The initial shell work also used corresponding reference copies in `app/ui/old-form-ui/`; current builds do not read those copies.

| WinForms source | Qt equivalent / interpretation |
| --- | --- |
| `Forms/frmMain.Designer.cs`, `Forms/frmMain.cs` | `QMainWindow`, four menus, three application-wide toolbars, 1129 × 571 initial client size, stacked Connections/Config docks and General panel. Notifications is expanded rather than auto-hidden. |
| `Menu/msMain/FileMenu.cs` | Shared file-action names; production New/Open/Save/Save as/Close connection file and explicit legacy conversion work. Preview file/storage actions and Options remain unavailable; its node edits are in-memory only. Save as connection is a separate production temporary-session action. |
| `Menu/msMain/SessionsMenu.cs` | Next/previous moves into View → Jump To; production Save as moves to File, different credentials to Tools. |
| `Menu/msMain/ViewMenu.cs` | Approved ordering: Add Connection Panel, Connection Panels; Connections/Config/Notifications/Screenshots; Jump To/reset/lock; Quick Connect/External Tools/Multi SSH toolbars; Full Screen. Jump targets existing widgets, Screenshots is disabled. |
| `Menu/msMain/ToolsMenu.cs`, `HelpMenu.cs` | External-tools and Help surfaces. External commands, URLs, updates and donations are disabled. About describes the preview. UltraVNC Single Click is omitted as it is hidden in the source. |
| `Controls/QuickConnectToolStrip.cs` | Global Connect label, editable history, protocol split-button and saved-connections menu. Choices match [`Connection/Protocol/ProtocolType.cs` at the pinned revision][upstream-protocols]; all create placeholders only. |
| `Controls/MultiSshToolStrip.cs` | Optional label/300-pixel command field. Read-only; command broadcast is disabled. |
| `Window/ConnectionTreeWindow.Designer.cs`, `StatusImageList.cs` | Single-selection promoted tree, four toolbar actions, search, original root/folder/PuTTY icons. Example connection icon is `Icons/mRemoteNG.ico`. No PuTTY configuration is read. |
| `Window/ConfigWindow.cs`, `Controls/ConnectionInfoPropertyGrid/ConnectionInfoPropertyGrid.cs` | Categorized/alphabetical read-only sample properties, four display modes, help, and disabled icon-picker/host-status buttons. Root/PuTTY selection restrictions remain. This is not the .NET property descriptor/editor system. |
| `Window/ConnectionWindow.Designer.cs`, `Tabs/ConnectionTab.Designer.cs` | Outer group tabs with independent inner session tabs. The dark surface explicitly says it has no network connection. |
| `Window/ErrorAndInfoWindow.Designer.cs`, `ErrorAndInfoWindow.cs` | Details/icon at left, message list at right, resizable splitter and selected/all Copy/Delete behavior. |

## Right-click menus

| Surface and original source | Working local commands | Unavailable operations |
| --- | --- | --- |
| Connections — `Controls/ConnectionContextMenu.cs` | Connect opens placeholders; folders open their descendant connections. Duplicate, Rename, Delete, New connection/folder/root, ascending/descending Sort and sibling Move work in memory. Copy hostname uses endpoint metadata, independent of display name. | Connection options, tree-level Disconnect, external execution, Wake-on-LAN, credential cache changes, inheritance writes, imports and export. |
| Session tab — `Window/ConnectionWindow.Designer.cs`, `ConnectionWindow.cs` | Rename, intentional Duplicate, Disconnect (close placeholder), disconnect other tabs and disconnect tabs to the right. | Reconnect, minimize-to-auto-hide and remote operations. RDP/VNC/SSH/PuTTY commands follow protocol visibility rules, but remain disabled. |
| Group tab — `Panels/PanelAdder.cs` | Rename and Close. View → Connection panels names/entries follow rename/deletion. | Send to another monitor. General cannot close, even after rename or tab reordering. |
| Config — `Window/ConfigWindow.cs` | Checkable Show Help Text. | Reset is shown but disabled because properties are read-only examples. |
| Notifications — `Window/ErrorAndInfoWindow.cs` | Copy/Delete selected messages; Copy All/Delete All when none are selected. | Both commands are disabled on an empty list. |
| External-tools toolbar — `Controls/ExternalToolsToolStrip.cs` | Checkable Show Text switches between icon-only and text-and-icon. Dock/toolbar visibility commands remain available. | External process execution. |

Menus follow source command order. Disabled commands have explanatory tooltips. Commands act on the clicked item/tab, including inactive tabs, rather than the previously active tab. Keyboard context-menu events use the current item/tab. Empty tree/tab-bar space does not reuse a stale destructive target.

The shared connection tree uses single selection. Notifications retains multiselection and right-click targeting. Tree actions resolve the clicked item and protect Connections/PuTTY roots; PuTTY editing stays disabled. Preview sibling Move actions remain in-memory context commands, not persistent drag-and-drop.

Rename dialogs and deletion confirmations are nonblocking and cancel without mutation. Tree deletion and closing a nonempty panel require confirmation. Movable-tab actions resolve their target page's current index when executed. Persistent model indexes and guarded page pointers prevent stale popup/dialog targets from accessing deleted objects.

No application popup is attached to the session surface: right-click there belongs to remote mouse input once a backend is integrated. Search, quick-connect and message-text controls keep their ordinary Qt editing menus.

## Local behavior and limits

Tree selection updates Config/help. Search is case-insensitive and retains matching ancestors. Node edits remain in memory; Favorites is absent from the four-button toolbar. Renaming a connection changes its display name, not its endpoint.

Double-click and quick connect open labeled local tabs. Repeating the same endpoint/protocol within a group selects an existing page, including after its caption changes. Duplicate deliberately creates another page. New panels have independent session tabs. Closing a page schedules deletion; closing a panel removes its View-menu entry and pages.

Dock close buttons and View toggles stay synchronized. Docks can float. Reset restores the left stack, full-width bottom pane and initial toolbar visibility. Lock affects the three application toolbars. Notifications update details for a single selection and clear stale details after deletion.

There are no socket workers, Rust bridge, credential provider, profile access, external process launcher or persisted settings in this preview. **Clipboard writes occur only when Copy hostname or notification Copy is explicitly selected.** There is no remote/automatic clipboard synchronization. Ordinary Qt text editing also retains its standard explicit Copy/Paste commands.

## Icons, provenance and licensing

`preview-icons.qrc` has **86 aliases for 85 unique upstream image files**: 47 aliases under `:/original/` (46 `Resources/*.png` files plus `Connection.ico`), and 39 under `:/connection-icons/` (the complete `Icons/*.ico` set). `Icons/mRemoteNG.ico` is shared by `:/original/Connection.ico` and `:/connection-icons/mRemoteNG.ico`; both remain for connection-profile compatibility. The obsolete `:/original/mRemoteNG_Icon.ico` application logo is excluded. Window/group branding uses the separate application-owned logo.

Named actions, toolbars, tree nodes, tabs, quick connect, search, Config status/icon controls and Notifications retain the upstream images. Commands that have no image in the original source remain text-only; a blank icon slot there is intentional.

Key mappings:

| UI | Original resources |
| --- | --- |
| Tree toolbar | `AddItem_16x`, `AddFolder_16x`, `ExpandAll_16x`, `SortAscending_16x` (the checkout's boxed-plus Expand icon, not pixel-identical to the supplied green-stack screenshot) |
| Tree/tab connection icons | `ASPWebSite_16x`, `FolderClosed_16x`, `PuttySessions`, `Icons/mRemoteNG.ico`; window/group branding is application-owned |
| Quick connect/search | `Run_16x`, `ASPWebSite_16x`, `Search_16x` |
| Config | `Property_16x`, `Schema_16x`, `ExtendedProperty_16x`, `ViewDownBySchema_16x`, selected-node icon and `HostStatus_Check` |
| Local edit/close/move | `Copy_16x`, `Rename_16x`, `Close_16x`, `GlyphUp_16x`, `GlyphDown_16x`, `Remove_16x`, `Add_16x` |
| Session remote surfaces | `FullScreen_16x`, `Resize_16x`, `Monitor_16x`, `Message_16x`, `Refresh_16x`, `SyncArrow_16x`, `ToggleOfficeKeyboardScheme_16x`, `PuttyConfig` |
| Main menus/panels | Source-assigned New/Open/Options/Exit, Help/Updates/About, external-tools and panel icons |
| Notifications | `StatusInformation_16x`, `Copy_16x`, `Close_16x` |

The .NET PropertyGrid's built-in categorized/alphabetical images are not standalone files at the pinned revision. Those two buttons use the original project's `SchemaObjectProperty_16x` and `SortAscending_16x` assets instead. Qt's native dock/toolbar/close-button decoration still comes from Qt; this is not a Windows caption-button port.

Two named standalone `.resx` byte arrays were extracted once during the assets-only migration, using the exact byte-array MIME type and strict base64. They are committed as image files; the wrapper no longer parses `.resx` or needs upstream source.

| Source, under [pinned `mRemoteNG/UI/`][upstream-ui] | Curated image | Use |
| --- | --- | --- |
| [`Window/ConnectionWindow.resx`][upstream-session-resources] / `cmenTabReconnect.Image` | `reference/reconnect.png` | Disabled session Reconnect command |
| [`Window/ConnectionWindow.resx`][upstream-session-resources] / `cmenTabExternalApps.Image` | `reference/external-tools.png` | Disabled session External applications command |

The former `Forms/frmMain.resx` / `$this.Icon` reference (`main.ico`) is obsolete and is absent from the curated set. Qt tests decode all **87 retained upstream images**: 47 resource paths under `:/original/` plus 39 under `:/connection-icons/`, sharing one file, and two reference PNGs. Application-owned branding is tested separately.

`vendor/mremoteng-assets/manifest.json` pins the upstream URL and commit `4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6`. Each image records its original source path, extraction entry when applicable, destination, SHA-256 and actual resource aliases. Unmodified `COPYING.txt`/`CREDITS.md` and local `NOTICE.txt` are also hashed. `scripts/ui_assets.py` rejects undeclared/missing files, changed hashes, unsafe paths, symlinks, invalid image signatures/dimensions/bounds, and disagreement between manifest and either qresource namespace. The preview stages this checked set with `manifest.json` and `provenance.json`. Package staging uses the same helper to retain notices and provenance without duplicating embedded image files. **No .NET code, object deserializer, `ImageListStreamer`, full upstream clone or network request is involved.**

The retained upstream [`COPYING.txt`][upstream-copying] contains GPL v2. [`CREDITS.md`][upstream-credits] credits the Silk Icon Set, copyright © 2005–2008 FAMFAMFAM, under Creative Commons Attribution 2.5, and PuTTY, copyright © 1997–2017 Simon Tatham, under MIT. These aggregate credits do not establish which license applies to every current image. Per-image rights must be checked before redistribution. Keeping only selected image bytes and notices does not resolve those rights. **Do not redistribute an icon-bearing binary/package until per-image rights are resolved.** No such package was produced or published during the historical production integration. RemoteManager's own license remains undecided.

Offline asset-only validation (no Qt build or SDK installation):

```sh
PYTHONDONTWRITEBYTECODE=1 python3 scripts/ui_assets.py
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p test_ui_assets.py
```

## Appearance and verification

The source uses Segoe UI 8.25 pt at 96 DPI. The preview requests Arial 9 pt, uses Qt Fusion and retains native OS window chrome. On macOS, application menus use the system menu bar at the top of the screen; other platforms and offscreen runs keep them inside the window. Widget captures do not include the macOS system menu bar. Its minimum is 640 × 480 rather than the source's 400 × 400. It is not a pixel-identical Windows port.

Standard `QDockWidget` does not provide DockPanelSuite's auto-hide/pin strips. Group/session detachment, monitor sending, preview drag/edit persistence and secondary forms remain outside this prototype. Production has editable delegates and persistent profile drag, but preview stays read-only for those paths. Reference themes/localization/model are not implemented Qt subsystems.

`ui_preview_test` includes:

- `sourceMenusAndDockGeometry`
- `previewSharesGridRootAndToolbarWithoutStorage`
- `searchAndPropertyHelp`
- `nestedTabsQuickConnectAndClose`
- `doubleClickAndMemoryOnlyTreeActions`
- `visibilityAndResetLayout`
- `contextMenuTargetsClickedObject`
- `treeContextMenuRestrictionsAndLocalActions`
- `sessionContextMenuActionsAndProtocolGating`
- `groupContextMenuLifetimeAndGeneralProtection`
- `propertyAndNotificationContextMenus`
- `externalToolbarContextAndRemoteSurface`
- `originalIconsAreCompleteAndDecode`
- `extractedImagesDecodeAsImages`

Tests cover pointer/keyboard context events, a popup action clicked through Qt mouse events, inactive/moved tabs, selected-subtree scope, root/PuTTY/General protection, rename/delete cancellation, stale targets, panel-menu cleanup, protocol gating, help/text toggles and all image decoding. The Cocoa run reads this process's actual AppKit menu, verifies the four top-level menus and revised View entries, and invokes View → Connections through that native menu. Checking only `isNativeMenuBar()` cannot detect an empty macOS menu. The test-only AppKit helper needs no desktop capture or cross-application automation permission. Copy assertions run only with the offscreen clipboard; native tests do not overwrite the host clipboard. They do not establish cross-application clipboard interoperability.

The receipts below are historical evidence from before the assets-only migration. Their references to the full checkout, old form copies, image totals and protected-file counts describe those earlier snapshots; source citations now resolve to the [pinned upstream revision][upstream-ui], not required local trees. They are not a rerun of the migrated build.

Original preview validation on 2026-10-05 passed macOS offscreen, native Cocoa and Linux ARM64 suites. Native tree/session menu captures were opened and checked; the isolated preview change preserved 1,633 protected files at that time.

Production integration validation on 2026-10-06 reran the preview suites on macOS offscreen and Linux ARM64 (13 functional tests each) alongside full production native/mock-only/Cocoa/Linux tests and seven owned live-RDP cases. T21 reran both preview suites with the vault controls hidden/disabled, alongside the vault suites and all eight live cases. Reference inputs, root `test.ui` and dependency locks remained unchanged. Generated preview output still stays in preview build directories. See [production validation](validation.md) for results and limits.

Final T27 validation on 2026-10-06 passed macOS offscreen, native Cocoa and Linux ARM64 preview suites: 17 Qt checks each, including extracted-image decoding with its asset directory supplied, and no failures/skips. The native Config-popup widget capture was opened and inspected. The complete production/mock/Cocoa/Linux/live matrix and final 1,613-file reference/pin audit also passed; see [production validation](validation.md) for counts and limits.

T35 preview checks on 2026-10-08 passed macOS offscreen, native Cocoa (18 Qt checks, zero failures/skips) and Linux ARM64 offscreen. The native Config-popup capture was opened and inspected. Preview Password remains display-only with no secret handler, networking or production-file access. The separate full production Cocoa activation failure keeps T35 in progress; see [current validation](validation.md).

Windows, Linux AMD64, accessibility, pixel parity and cross-desktop docking behavior are not validated by this work.

[upstream-ui]: https://github.com/mRemoteNG/mRemoteNG/tree/4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6/mRemoteNG/UI
[upstream-protocols]: https://github.com/mRemoteNG/mRemoteNG/blob/4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6/mRemoteNG/Connection/Protocol/ProtocolType.cs
[upstream-session-resources]: https://github.com/mRemoteNG/mRemoteNG/blob/4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6/mRemoteNG/UI/Window/ConnectionWindow.resx
[upstream-copying]: https://github.com/mRemoteNG/mRemoteNG/blob/4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6/COPYING.txt
[upstream-credits]: https://github.com/mRemoteNG/mRemoteNG/blob/4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6/CREDITS.md
