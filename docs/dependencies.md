# Dependencies and licenses

CMRemote is licensed **GPL-2.0-or-later**; see [`LICENSE`](../LICENSE). Packages retain the third-party notices below. Published installers are built by the GitHub release workflow.

## Pinned stack

| Dependency | Version | Purpose / license |
|---|---|---|
| Qt | 6.12.0 | Widgets/Core/Gui/Test; LGPL-3.0 / GPL-3.0 / commercial alternatives, depending on module and license choice |
| FreeRDP / WinPR | 3.32.1 | Embedded RDP and channels; Apache-2.0, plus third-party notices in its source |
| OpenSSL | 3.5.9 | FreeRDP TLS + connection XML PBKDF2/AES-256-GCM; Apache-2.0 |
| zlib | 1.3.2 | Compression; zlib license |
| cxx / cxx-build | 1.0.202 | FFI bridge; MIT OR Apache-2.0 |
| vnc-rs | 0.6.0, vendored and locally patched | Embedded classic VNC/RFB; MIT OR Apache-2.0 |
| Rust | 1.99.0 | Compiler/toolchain; MIT OR Apache-2.0, with toolchain third-party notices |
| serde | 1.0.228 | JSON model serialization; MIT OR Apache-2.0 |
| serde_json | 1.0.151 | Password-free metadata and SSH trust codec; MIT OR Apache-2.0 |
| uuid | 1.27.0 | Stable IDs; MIT OR Apache-2.0 |
| tempfile | 3.23.0 | Temporary Rust test fixtures; MIT OR Apache-2.0 |
| CMake | 4.4.4 | Build generation; BSD-3-Clause, with bundled notices |
| Ninja | 1.13.2 | Build execution; Apache-2.0 |
| py7zr | 1.0.0 | SDK archive extraction only; LGPL-2.1-or-later and separately licensed dependencies |
| ICU | 73.2 in the Linux Qt archive | Qt Unicode runtime; Unicode/ICU license and notices |

`Cargo.lock` pins the full Rust dependency graph and registry checksums; Cargo does not assign a registry checksum to local path crates. `dependencies.lock.json` records exact SDK/source URLs and SHA-256 values for both Linux architectures and macOS, plus the approved vnc-rs vendored path, original archive and attribution files. `scripts/bootstrap-requirements.txt` pins the Python extractor dependency closure. Python wheels are version-pinned, not hash-locked; apt repositories are not snapshot-pinned. This is not a fully hermetic supply-chain build.

The bootstrap verifies downloaded and cached SDK/source bytes before extraction. Rustup is contained using supported `CARGO_HOME`/`RUSTUP_HOME` variables and `--no-modify-path`; it verifies toolchain downloads through Rust distribution metadata. Cargo verifies locked registry downloads. Existing Apple compiler/SDK versions are host prerequisites, not project-installed artifacts.

## Native library strategy

Use checksummed prebuilt Qt/CMake/Ninja, one SDK per native target. macOS Qt is the official universal qtbase archive; the application itself is built for the host architecture. Linux uses native qtbase + qtwayland + ICU archives. All targets add the qtshadertools archive for the build-time `qsb` shader compiler only; QSB deserialization at runtime lives in QtGui. No QML or unrelated Qt module pack is installed.

OpenSSL, zlib, OpenH264 and FreeRDP are source-built into `.dev/macos/prefix` or the corresponding Linux image prefix. The OpenH264-enabled SDK/application builds have executed on macOS and Linux ARM64. Both full suites pass encoder/decoder context creation and reset; live AVC acceptance remains pending. FreeRDP common/client libraries and CLIPRDR/DISP/RDPGFX channels are linked directly. External FreeRDP GUI clients and servers are disabled. Audio, USB, device/smart-card redirection, FFmpeg, CUPS, FUSE and Kerberos-related integrations are disabled. The runtime also disables audio/device redirection and RDP8 options that would force the omitted rdpdr addin. Linux WinPR uses its built-in Unicode implementation; Qt still uses its bundled ICU runtime.

Connection XML crypto explicitly links `OpenSSL::Crypto` in full and mock-only production builds. It uses the existing 3.5.9 PBKDF2-HMAC-SHA1 and AES-256-GCM APIs for native mRemoteNG compatibility. The former Argon2id vault and its production conversion routes have been removed; they are not current storage APIs. Native/Linux tests verify independent cipher vectors and XML parameter limits, not an original .NET runtime. The standalone preview links Qt only and does not load production credentials. See [connection password format and limits](password-vault.md).

The development image remains pinned to the Ubuntu 24.04 multiarch digest in `Dockerfile`. Apt installs compilers, graphics/runtime libraries and test tools inside the image only. `dpkg-query -W` records their resolved versions in `/opt/remotemanager/.dev/linux/native-packages.txt`.

The optional owned xrdp fixture uses a separate Ubuntu 26.04 image pinned to `sha256:f144425ff09be612d6d9ad965196e9cdc23dae1f42110a8a11a3e9a8198759f7`. Validation resolved xrdp `0.10.1-4.1` and xorgxrdp `1:0.10.2-1build1`; the runner saves its full `/fixture-packages.txt` inventory as `build/macos/rdp-fixture-packages.txt`. This test-only server update fixes resize output ordering in the older Ubuntu 24.04 xrdp 0.9.24 fixture. It does not change application SDK pins, Cargo.lock, the development base or host tools. Apt repositories are still not snapshot-pinned. xrdp uses Apache-2.0; Xorg/Tk and other fixture dependencies retain their own licenses.

## Isolated T36 SDK — 2026-10-09

`build/macos/test-data/t36-candidate` selects only Qt ShaderTools additions beyond `ce1b429`. Its copied base Qt tree was missing ShaderTools before candidate bootstrap installed and validated qsb 6.12.0. Original Qt SDK remains unchanged. Candidate FreeRDP was rebuilt with the committed bootstrap flags: OpenH264/FFmpeg/client/server disabled, full-image worker and GFX-disabled runtime retained. This differs from the later decoder-enabled working SDK below.

Runtime audit found pre-existing host Homebrew uriparser/jansson dependencies and OpenSSL install names resolving to the original project-contained prefix. No host packages were installed or changed. This candidate is not a relocated, self-contained distribution; no packaging or release claim follows from its test results.

## Test-only SDL comparator and decoder source audit — T37/T39

`dependencies.lock.json` separately pins SDL3 3.2.26, SDL3_ttf 3.2.2, FreeType 2.13.3 and OpenH264 2.6.0 source archives and SHA-256 values. The offline verifier checks their exact source APIs, dependency options and license files. OpenH264 2.6.0 is built and directly linked into the macOS and Linux ARM64 production FreeRDP SDKs. The full library includes encoder exports and installed codec headers. `USE_ASM=No` avoids an unpinned NASM prerequisite; its performance cost has not been measured. Runtime codec loading, FFmpeg/swscale, Media Foundation, MediaCodec and hardware-decoder paths are disabled. Successful encoder/decoder context creation and reset are unit evidence, not live AVC negotiation.

SDK reuse requires matching version/source/configuration contracts and installed file hashes in `.rm-openh264.json` and `.rm-freerdp.json`; a decoder-less header cannot authorize reuse. FreeRDP CMake cache values and installed feature macros are also checked. Packages stage the exact installed OpenH264 license and a source-build/patent-clearance notice inside the artifact. macOS staging removes mutable SDK/build `LC_RPATH` entries, audits dependency/search paths and signs again after relocation.

`python3 scripts/bootstrap-comparator.py --platform macos` builds SDL-FreeRDP 3.32.1 under `.dev/macos/comparator/prefix`, with source under `.dev/macos/comparator/src` and build files under `build/macos/comparator`. It never replaces `.dev/macos/prefix`. Linux uses `.dev/linux/comparator/{src,prefix}` and `build/linux/<arch>/comparator`, requires native Linux execution, and selects the caller's `RM_NATIVE_PREFIX`/`RM_QT_PREFIX`/`RM_BUILD_DIR` rather than assuming the bind-mounted application SDK. The Linux ARM64 build and version check pass. X11/Wayland development prerequisites are supplied by the development image; fresh source downloads use a contained cache. Child library search paths put the comparator prefix first. The ELF audit resolved FreeRDP/WinPR/SDL/FreeType to that prefix and QtCore/OpenSSL/zlib to the selected SDK, with no missing libraries. SDL enumerates both X11 and Wayland and has executed native final-pixel diagnostics on each. These checks do not establish hardware rendering, a repeatable accepted performance baseline or SDL parity. SDL is a separate test process, never embedded or reparented into the application.

The comparator is Release, with OpenH264/FFmpeg and the same omitted audio/device integrations disabled. Its owned-fixture hooks explicitly match those runtime disable settings before channel loading. The application frontend is Debug; its FreeRDP library is Release. Test-only probes link the already pinned QtCore solely to reuse the bounded numeric trace implementation. That build difference is recorded rather than presented as an identical executable configuration. The installed executable's `/version` is checked after installation; a successful compile alone does not verify runtime library resolution.

SDL3 and SDL_ttf use the zlib-style license. SDL_ttf requires SDL >=3.2.6 and FreeType; its vendored downloads, HarfBuzz, plutosvg and samples are disabled. FreeType's optional zlib, PNG, bzip2, HarfBuzz and Brotli dependencies are disabled. The comparator selects the FreeType License (FTL) and retains both its license and acknowledgment obligations. Notices are copied to `share/remotemanager-comparator-notices` in the separate prefix. Portions of this test software are copyright © 2024 The FreeType Project (www.freetype.org). All rights reserved.

The OpenH264 2.6.0 source license permits source/binary redistribution with the stated copyright, conditions and disclaimer. It does not establish H.264 patent coverage or eligibility for Cisco's separately supplied binary coverage. The full library is required for FreeRDP's adapter, including encoder exports and `codec_ver.h`, even when this app only intends software decoding.

**Packages contain no OpenH264 (2026-10-16).** The source build above only supplies FreeRDP's headers and the test stand-in. FreeRDP is built with `WITH_OPENH264_LOADING=ON`. `scripts/bootstrap.py` (`patch_freerdp`) replaces its fixed library-name search with `freerdp_openh264_set_library`, so FreeRDP loads only the path the app supplies; with none, H.264 is not negotiated. `app/OpenH264.cpp` follows Cisco's [binary license](../vendor/licenses/OpenH264-Cisco-BINARY_LICENSE.txt) conditions:
- **Separate download:** each device downloads Cisco's prebuilt 2.6.0 binary itself from `http://ciscobinary.openh264.org/`. The bzip2 file is decompressed by the pure-Rust `libbz2-rs-sys`, checked against a pinned SHA-256 and saved in the application-data `openh264/` folder.
- **Pinned checksums:** each pinned SHA-256 was taken from a binary whose MD5 matched Cisco's signed list.
- **User control:** **Options → Remote Desktop → Use H.264 video** turns it on (the default), off or back on. The text "OpenH264 Video Codec provided by Cisco Systems, Inc." appears beside that setting.
- **License text:** packages carry Cisco's full binary license.

`scripts/package.py` refuses any artifact containing an OpenH264 library. Cisco publishes macOS arm64/x64, Linux x86_64/arm64 and Windows x64 binaries; other platforms run without H.264. Cisco's patent notice covers personal, non-commercial use; other uses may need a separate license. A future hardened-runtime macOS signature needs the `com.apple.security.cs.disable-library-validation` entitlement to load Cisco's separately signed library.

Primary audit sources: [SDL3 license](https://github.com/libsdl-org/SDL/blob/release-3.2.26/LICENSE.txt), [SDL_ttf build](https://github.com/libsdl-org/SDL_ttf/blob/release-3.2.2/CMakeLists.txt), [FreeType 2.13.3 source](https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz) (`docs/FTL.TXT`, `LICENSE.TXT`, `CMakeLists.txt`), [OpenH264 license](https://github.com/cisco/openh264/blob/v2.6.0/LICENSE) and [build](https://github.com/cisco/openh264/blob/v2.6.0/Makefile).

## Curated original UI assets — redistribution blocked

Production and the standalone preview compile `app/ui/test.ui` with `app/ui/preview-icons.qrc`. That catalog now reads only `vendor/mremoteng-assets/`: **86 aliases for 85 unique images** (46 `Resources/*.png` plus 39 `Icons/*.ico`). The actual namespaces are `:/original/` (47 aliases, including the compatibility `Connection.ico`) and `:/connection-icons/` (39 aliases). Both namespaces share `Icons/mRemoteNG.ico`, which is retained as a connection-profile icon. Only the old application logo `Resources/mRemoteNG_Icon.ico` and its alias were removed; application-owned branding is separate. Two exact standalone byte arrays from named `.resx` entries are retained as `reference/reconnect.png` and `reference/external-tools.png`, for **87 upstream images total**. The old `main.ico` extraction is no longer an input.

`manifest.json` pins <https://github.com/mRemoteNG/mRemoteNG.git> at commit `4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6`, original paths/extraction entries, destinations, SHA-256 values and resource aliases. `scripts/ui_assets.py` validates containment, absence of symlinks, exact file membership, hashes, bounded PNG/ICO structures and both catalog namespaces before staging. Production packages retain the same checked notices, manifest and provenance through `stage_assets(..., include_images=False)`; image bytes are already embedded by Qt. Preview stages the images as well and reads the two references at `RM_PREVIEW_ASSET_DIR`. Neither build needs a full upstream clone, .NET runtime, original source code or `.resx` parsing. `app/ui/old-form-ui/` is also unused. See [resource mappings and provenance](ui-preview.md#icons-provenance-and-licensing).

The subset retains byte-identical upstream [`COPYING.txt`](https://github.com/mRemoteNG/mRemoteNG/blob/4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6/COPYING.txt) (GPL v2) and [`CREDITS.md`](https://github.com/mRemoteNG/mRemoteNG/blob/4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6/CREDITS.md), plus a local `NOTICE.txt`. Aggregate credits include FAMFAMFAM Silk under CC BY 2.5 and PuTTY under MIT; they do not establish each image's rights. Icon-bearing packages redistribute these images under the upstream GPL v2 distribution with its notices; per-image provenance remains unresolved, and curating files and retaining attribution is not a per-image legal clearance.

T12–T17's statement that no icon-bearing package was produced or published is historical integration evidence. Earlier package smoke results apply only to that earlier application, not the current shell or this migration. Historical references elsewhere to full local source trees or older image counts describe those snapshots; current research citations refer to the [pinned upstream revision](https://github.com/mRemoteNG/mRemoteNG/tree/4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6), not build dependencies.

## Qt obligations before distribution

This code dynamically links Qt. An LGPL distribution generally needs the appropriate license notices/texts, access to matching Qt source and any modifications, and the ability for recipients to replace/relink the LGPL library as required by the license. Signing, platform restrictions and installer design must not defeat those rights. Qt's third-party fonts, image libraries and other bundled components also need their notices. Commercial Qt licensing is a separate option, not assumed here.

`macdeployqt` and the Linux staging script copy runtime libraries/plugins; they do not settle all legal or distribution obligations. SDK SBOM files are under `.dev/macos/qt/sbom` or the Linux image SDK. Read the exact module and third-party licenses for a release, retain matching source/build information, and decide the application license before publishing a bundle.

## Embedded SSH2 dependencies — T48, 2026-10-11

| Dependency | Version | Purpose / license |
|---|---|---|
| russh | 0.64.1 | SSH2 transport; Apache-2.0 |
| Tokio | 1.48.0 | Worker runtime; MIT |
| ring | 0.17.14 | Selected russh crypto backend; Apache-2.0 AND ISC, with bundled third-party notices |
| libc | 0.2.190 | Direct Unix dependency for nonblocking/no-follow key-file opens; MIT OR Apache-2.0 |
| libvterm | 0.3.3 | Embedded terminal emulation, static C99 library; MIT |

Cargo selects russh with default features disabled and only `ring`/`rsa` enabled. The locked closure includes ssh-key 0.7.0-rc.11 and RSA 0.10.0-rc.18; those prereleases are explicit, not assumed stable APIs. No AWS-LC or russh compression feature is enabled. ssh-key includes its upstream PPK decoder and Tokio's unified features include `rt-multi-thread` through russh; this does not promise PPK support or require the application worker to use a multithread runtime. Windows Pageant and other target-only packages remain in the lock, without agent integration or platform acceptance claims.

The all-target Cargo metadata audit resolved 197 registry packages plus the local Core. Their archive checksums, declared license expressions and retained notice files are checked by `scripts/rust_notices.py`. Permissive alternatives are available for the OR-licensed packages; ring's AND expression and unicode-ident's Unicode notice are retained. r-efi keeps its embedded MIT permission/copyright in `AUTHORS`. delegate, Pageant and the three russh crates omit license files from their published archives: `dependencies.lock.json` pins additional notices to the source revisions recorded in `.cargo_vcs_info.json`. russh-util's published metadata marks its source tree dirty; the retained upstream Apache text does not claim an exact clean source-tree reproduction.

libc 0.2.190 was already in the audited locked closure. Its direct Unix edge adds no registry package or version change. The exact manifest and platform constants supply `O_NONBLOCK`, `O_NOFOLLOW` and `O_CLOEXEC` rather than application-defined flag values; the complete notice audit was rerun after the edge changed. Windows key-file loading remains untested.

Build-script/target inspection identified ring's native C/assembly compilation, compiler capability probes, and target-specific platform library paths. The published ring archive uses pregenerated assembly without a new Perl/NASM installation. Cargo's target closure, source checksums and notices are dependency evidence, not a vulnerability audit, verified SSH behavior or distribution clearance.

libvterm builds from the checksummed official archive's nine C files and generated include tables. SDK reuse checks version/source/architecture, the project CMake adapter hash and installed file hashes, then runs a static link/version/cell probe. That probe and production application linkage pass on macOS ARM64 and Linux ARM64. Root CMake requires exact VTerm 0.3.3 and QtNetwork from the existing qtbase, including builds without FreeRDP. The Linux image copies the CMake adapter before SDK bootstrap. Production and full/mock-only regression builds have passed on both platforms. The old RDP integration test has been migrated to the current XML/file-password flow rather than restoring its deleted vault dependency.

The Linux development image includes `openssh-client` only as test tooling for synthetic, owned OpenSSH key generation; the observed version is `1:9.6p1-3ubuntu13.19`, retained in the resolved native-package inventory. Image creation accepts the wrapper's numeric UID/GID and creates an account/group only when that ID is absent. This lets OpenSSH resolve the bind-mounted workspace's runtime user without a `HOME` override or host-account modification. Apt repositories remain unsnapshotted. A separate owned OpenSSH/vim/tmux fixture, based on the pinned Ubuntu development base, records its resolved packages and exposes only random loopback ports. T50 adds `tmux` only to this test fixture (`3.4-1ubuntu0.1` in both retained package inventories), not the production runtime or host. All eight production SSH acceptance cases passed with native macOS Cocoa and Linux ARM64 XCB. Tests use generated secrets/keys and fixture-side oracles, never user accounts, host sshd settings or `~/.ssh/known_hosts`. The trust codec uses existing serde/serde_json and ssh-key APIs; it adds no Rust package/version change.

Packaging stages the checked installed libvterm MIT notice and all 197 locked Rust notice sets under the artifact's third-party directory. The libvterm SDK headers/static archive are not runtime package dependencies. Relocated macOS ARM64 and Linux ARM64 development artifacts passed static libvterm symbol/notice audits and shipping startup checks. macOS also passed ad-hoc signature and contained dyld-resolution checks; Linux passed SDK-masked, network-disabled artifact-local runtime/crypto and XCB RHI checks. These probes do not test packaged SSH-to-server networking. Windows, AMD64 and Wayland SSH remain untested. Appearance preferences use existing Qt Widgets/JSON and AtomicFile APIs with no new dependency. alacritty_terminal is not used; the user's libvterm choice supersedes that earlier proposal. See [validation receipts and limits](validation.md). Redistribution remains blocked by the undecided project license and unresolved original-icon rights.

Primary sources: [russh archive](https://static.crates.io/crates/russh/russh-0.64.1.crate), [Tokio archive](https://static.crates.io/crates/tokio/tokio-1.48.0.crate), [ring archive](https://static.crates.io/crates/ring/ring-0.17.14.crate), [official libvterm archive](https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz), the exact revision notice URLs in `dependencies.lock.json`, and each locked crate's manifest/build script.

## Embedded VNC dependency — 2026-10-12

The approved implementation uses **vnc-rs 0.6.0**, declared `MIT OR Apache-2.0`, as an exact local path dependency at `vendor/vnc-rs`. The original published archive is retained at `vendor/vnc-rs-0.6.0.crate` and pinned in `dependencies.lock.json`:

- Source: <https://static.crates.io/crates/vnc-rs/vnc-rs-0.6.0.crate>
- SHA-256: `0b757c55558e81e08e60f1a3153ba5bcffe7196719bc885e7e620400b0d81a4d`
- Attribution/license files: `vendor/vnc-rs/LICENSE-MIT` and `LICENSE-APACHE`, unchanged from that archive.
- Local provenance and changes: `vendor/vnc-rs/SOURCE-PROVENANCE` and `PATCHES.md`, maintained with the vendored source and its local patch. The archive digest identifies the **upstream baseline**, not a byte-identical patched source tree.

`scripts/rust_notices.py` previously skipped all Cargo packages with `source = null`. It now exempts only this project's Core and accepts the reviewed vnc-rs name/version/path/license contract explicitly; other unreviewed path packages fail closed. It checks the retained original archive digest, the local crate manifest and exact matching upstream license bytes, then retains both license texts and local provenance/patch notes. The ordinary dependency verifier also checks that vendored baseline and attribution without fetching or extracting over the patched tree. `cargo metadata --locked --offline` is still collected **without a target filter**, so build dependencies and Windows/other target-only registry packages remain in the checksum/license/notice audit. This is not a vulnerability/security audit or acceptance of a platform merely because it appears in Cargo metadata.

The existing package-stage caller uses the same notice collector: macOS artifacts retain VNC attribution under `Contents/Resources/third-party/rust/vnc-rs-0.6.0`; Linux uses `share/third-party/rust/vnc-rs-0.6.0`. Its generated `manifest.json` records the original archive checksum/source URL and hashes of staged notices, including the local patch/provenance information. No Windows packager/wrapper exists. A new staged artifact must rerun the complete updated locked-closure audit; the **197-package T48 results above are historical SSH evidence**, not the VNC closure or VNC-package evidence.

The client uses direct, unencrypted classic RFB 3.3/3.7/3.8; None is selected whenever the server offers it; otherwise VncAuth uses the reviewed saved Password or a session-only prompt, limited to 1–8 printable ASCII. Raw/ZRLE/CopyRect, server resize/cursors, view-only and shared native QRhi presentation are implementation scope, not accepted runtime evidence. TLS/VeNCrypt/vendor authentication, clipboard, client resize and proxy/chat/file transfer are excluded. Native build/live-server/graphics/fullscreen/package acceptance remains pending on every target; Windows source portability is pending without native evidence. See [VNC use and exclusions](../README.md#vnc-desktop--implementation-awaiting-acceptance) and [validation status](validation.md#vnc-client-integration--pending-acceptance-2026-10-12).

Retaining MIT/Apache attribution does not by itself settle original-icon provenance, Qt LGPL obligations or OpenH264 patent coverage; those remain the publisher's responsibility for each release. LibVNCClient is not linked or substituted by this work.

## Previously researched VNC alternative — not linked

| Dependency | Researched version | Planned role / license |
|---|---|---|
| LibVNCClient (LibVNCServer) | 0.9.15 | VNC; **GPL-2.0-or-later** |

LibVNCClient is not a permissively licensed drop-in. Linking it into a distributed application can impose GPL obligations on the combined work. Do not add it until the project's license/distribution plan is compatible or a different VNC approach is chosen.

## Audit commands

```sh
python3 scripts/verify-dependencies.py --fetch --platform macos
python3 scripts/verify-dependencies.py --offline --platform macos
python3 scripts/project.py test
```

The first command fetches only audit material into `.dev/macos/downloads`, not an SDK installation. Linux audit/test commands must run in the Linux workflow. The one-time `resolve-dependencies.py` is a maintainer research helper that expects previously fetched GitHub release metadata; normal bootstrap uses the checked-in lock, not this helper or mutable “latest” resolution.

## Upstream evidence and license references

- [FreeRDP 3.32.1 source](https://github.com/FreeRDP/FreeRDP/tree/3.32.1), [release archives](https://pub.freerdp.com/releases/), [Apache license](https://github.com/FreeRDP/FreeRDP/blob/3.32.1/LICENSE).
- [Qt SDK metadata](https://download.qt.io/online/qtsdkrepository/mac_x64/desktop/qt6_6120/qt6_6120/Updates.xml), [Qt licensing](https://doc.qt.io/qt-6/licensing.html), [LGPL obligations](https://www.qt.io/licensing/open-source-lgpl-obligations).
- [cxx](https://github.com/dtolnay/cxx), [Rust distribution](https://static.rust-lang.org/dist/), [Rust licenses](https://www.rust-lang.org/policies/licenses).
- [OpenSSL source/license](https://github.com/openssl/openssl), [zlib](https://zlib.net/), [CMake](https://cmake.org/), [Ninja](https://github.com/ninja-build/ninja).
- [russh](https://github.com/warp-tech/russh), [libvterm](https://www.leonerd.org.uk/code/libvterm/), [LibVNCServer/LibVNCClient license](https://github.com/LibVNC/libvncserver/blob/LibVNCServer-0.9.15/COPYING).
