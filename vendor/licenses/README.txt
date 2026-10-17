Third-party libraries bundled in CMRemote packages

Qt 6.12.0 (qtbase)   LGPL-3.0-only; Qt-LGPL-3.0.txt and Qt-GPL-3.0.txt
                     Source: https://download.qt.io/official_releases/qt/6.12/6.12.0/submodules/
                     Qt is linked dynamically and may be replaced. Qt's own bundled
                     components are listed in the qt-sbom/ SPDX files beside this file.
FreeRDP 3.32.1       Apache-2.0; FreeRDP-LICENSE.txt
                     Source: https://pub.freerdp.com/releases/freerdp-3.32.1.tar.gz
OpenSSL 3.5.9        Apache-2.0; OpenSSL-LICENSE.txt
                     Source: https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
zlib 1.3.2           zlib license; zlib-LICENSE.txt
                     Source: https://zlib.net/zlib-1.3.2.tar.gz

OpenH264 is not included. CMRemote downloads Cisco's prebuilt binary at runtime;
Cisco's binary license is OpenH264-Cisco-BINARY_LICENSE.txt.
libvterm, Rust crates and mRemoteNG assets have their own notices
in this directory. Exact pinned versions are in dependency-versions.json.
CMRemote's source, including build scripts, is at https://github.com/Crayjones/CMRemote.
