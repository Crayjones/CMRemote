<p align="center">
  <img src="app/assets/branding/cmremote-128.png" alt="CMRemote wireless monogram" width="128" height="128">
</p>

<h1 align="center">CMRemote</h1>
<p align="center">Cross-platform Multi Remote</p>

CMRemote is an open-source, tabbed remote connection manager for macOS, Windows and Linux, inspired by [mRemoteNG](https://github.com/mRemoteNG/mRemoteNG). Keep your remote desktops and terminals together in one application.

## Features

- **RDP** — remote desktops with H.264/AVC support.
- **SSH** — embedded terminal with password and private-key authentication.
- **VNC** — classic VNC/RFB connections.
- Organize connections in folders with inherited settings.
- Open mRemoteNG connection XML files, with optional file-password protection.
- Arrange sessions in tabs, panels and separate windows.

## Download

See [GitHub Releases](https://github.com/Crayjones/CMRemote/releases) for downloads.

| Platform | Package |
|---|---|
| macOS 14.4+, Apple silicon or Intel | `.dmg` |
| Windows x64 | Installer or portable `.zip` |
| Linux x86_64 | AppImage or Flatpak |

Builds are not yet signed with Apple or Microsoft certificates. See the release notes for installation instructions.

## Getting started

1. Create a connection file or open an existing mRemoteNG XML file from **File**.
2. Add a connection, choose RDP, SSH or VNC, and enter its host and account details.
3. Connect to open a session tab, or use **Quick Connect** for a temporary session.

H.264 uses Cisco OpenH264, downloaded separately on first launch. Control it under **Options → Remote Desktop**. Classic VNC traffic is unencrypted; use it only on trusted networks or through a secure tunnel.

## Build from source

CMRemote uses Qt 6 Widgets, C++ and Rust. No .NET runtime or mRemoteNG checkout is needed.

On macOS, install Apple's Command Line Tools, then run:

```sh
./scripts/bootstrap-macos.sh --jobs 8
python3 scripts/project.py build
python3 scripts/project.py run
```

For Linux, Windows cross-builds, tests and packaging, see the [development guide](docs/development.md).

## Documentation and contributing

- [Development and usage](docs/development.md)
- [Architecture](docs/architecture.md)
- [Dependencies and third-party notices](docs/dependencies.md)
- [Report a bug or request a feature](https://github.com/Crayjones/CMRemote/issues)

Bug reports, fixes and improvements are welcome. Include your operating system, CMRemote version and steps to reproduce; do not include passwords or private connection files.

## License

[GPL-2.0-or-later](LICENSE). Third-party components and retained mRemoteNG assets have their own [licenses and notices](docs/dependencies.md).
