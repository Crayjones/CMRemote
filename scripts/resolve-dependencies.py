#!/usr/bin/env python3
"""One-time upstream audit. Writes the reviewed lock; not used during bootstrap."""
import hashlib
import importlib.util
import json
from pathlib import Path
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.dev/macos/downloads'
CACHE.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('audit', ROOT / 'scripts/verify-dependencies.py')
audit = importlib.util.module_from_spec(spec); spec.loader.exec_module(audit)


def fetch(url, name):
    path = CACHE / name
    if not path.exists():
        path.write_bytes(audit.download(url))
    data = path.read_bytes()
    return {'url': url, 'file': name, 'sha256': hashlib.sha256(data).hexdigest()}


lock = {'schema_version': 1, 'versions': {
    'rust': '1.99.0', 'cxx': '1.0.202', 'qt': '6.12.0', 'cmake': '4.4.4',
    'ninja': '1.13.2', 'freerdp': '3.32.1', 'openssl': '3.5.9', 'zlib': '1.3.2',
    'aqtinstall': '3.3.0', 'py7zr': '1.0.0',
    'russh': '0.64.1', 'tokio': '1.48.0', 'ring': '0.17.14', 'libvterm': '0.3.3', 'libvncclient_planned': '0.9.15'},
    'artifacts': {}}
a = lock['artifacts']
# Reviewed notices omitted by upstream crate archives retain their exact revision pins.
previous = json.loads((ROOT / 'dependencies.lock.json').read_text())
for key, artifact in previous['artifacts'].items():
    if key.startswith('rust-notice-'):
        audit.fetch(artifact, CACHE)
        a[key] = artifact

a['freerdp'] = fetch('https://pub.freerdp.com/releases/freerdp-3.32.1.tar.gz', 'freerdp.tar.gz')
a['openssl'] = fetch('https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz', 'openssl.tar.gz')
a['zlib'] = fetch('https://zlib.net/zlib-1.3.2.tar.gz', 'zlib.tar.gz')
for key, version, url, filename in (
    ('libvterm', '0.3.3', 'https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz', 'libvterm-0.3.3.tar.gz'),
    ('russh', '0.64.1', 'https://static.crates.io/crates/russh/russh-0.64.1.crate', 'russh-0.64.1.crate'),
    ('sdl3', '3.2.26', 'https://github.com/libsdl-org/SDL/releases/download/release-3.2.26/SDL3-3.2.26.tar.gz', 'SDL3-3.2.26.tar.gz'),
    ('sdl3_ttf', '3.2.2', 'https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-3.2.2.tar.gz', 'SDL3_ttf-3.2.2.tar.gz'),
    ('freetype_comparator', '2.13.3', 'https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz', 'freetype-2.13.3.tar.xz'),
    ('openh264', '2.6.0', 'https://codeload.github.com/cisco/openh264/tar.gz/refs/tags/v2.6.0', 'openh264-2.6.0.tar.gz'),
):
    lock['versions'][key] = version
    a[key] = fetch(url, filename)
for package, version in [('cxx', '1.0.202'), ('cxx-build', '1.0.202')]:
    a[package] = fetch('https://static.crates.io/crates/{0}/{0}-{1}.crate'.format(package, version), package + '.crate')
a['rust-manifest'] = fetch('https://static.rust-lang.org/dist/channel-rust-1.99.0.toml', 'rust-1.99.0.toml')
for package in ['cmake', 'ninja']:
    data = json.loads((CACHE / (package + '.json')).read_text())
    for asset in data['assets']:
        name = asset['name']
        if package == 'cmake':
            platforms = {'cmake-4.4.4-macos-universal.tar.gz': 'macos',
                         'cmake-4.4.4-linux-aarch64.tar.gz': 'linux-arm64',
                         'cmake-4.4.4-linux-x86_64.tar.gz': 'linux-amd64'}
        else:
            platforms = {'ninja-mac.zip': 'macos', 'ninja-linux-aarch64.zip': 'linux-arm64', 'ninja-linux.zip': 'linux-amd64'}
        if name in platforms:
            a[package + '-' + platforms[name]] = {'url': asset['browser_download_url'], 'file': name,
                                                  'sha256': asset['digest'].removeprefix('sha256:') if hasattr(str, 'removeprefix') else asset['digest'][7:]}
for platform, triplet in [('macos-arm64', 'aarch64-apple-darwin'), ('macos-amd64', 'x86_64-apple-darwin'),
                          ('linux-arm64', 'aarch64-unknown-linux-gnu'), ('linux-amd64', 'x86_64-unknown-linux-gnu')]:
    a['rustup-' + platform] = fetch('https://static.rust-lang.org/rustup/archive/1.28.2/' + triplet + '/rustup-init', 'rustup-init-' + platform)
for platform, host, arch in [('macos', 'mac_x64', 'clang_64'), ('linux-arm64', 'linux_arm64', 'linux_gcc_arm64'), ('linux-amd64', 'linux_x64', 'linux_gcc_64')]:
    base = 'https://download.qt.io/online/qtsdkrepository/' + host + '/desktop/qt6_6120/qt6_6120/'
    meta = fetch(base + 'Updates.xml', 'qt-' + host + '-updates.xml')
    a['qt-metadata-' + platform] = meta
    tree = ET.parse(CACHE / meta['file'])
    target = 'qt.qt6.6120.' + arch
    pkg = next(p for p in tree.findall('PackageUpdate') if p.findtext('Name') == target)
    for archive in [s.strip() for s in pkg.findtext('DownloadableArchives').split(',')]:
        module = archive.split('-')[0]
        if module in ('qtbase', 'qtwayland', 'icu'):
            a[module + '-' + platform] = fetch(base + target + '/' + pkg.findtext('Version') + archive, module + '-' + platform + '.7z')
    addon = 'qt.qt6.6120.addons.qtshadertools.' + arch
    apkg = next(p for p in tree.findall('PackageUpdate') if p.findtext('Name') == addon)
    for archive in [s.strip() for s in apkg.findtext('DownloadableArchives').split(',')]:
        if archive.startswith('qtshadertools-'):
            a['qtshadertools-' + platform] = fetch(base + addon + '/' + apkg.findtext('Version') + archive, 'qtshadertools-' + platform + '.7z')
(ROOT / 'dependencies.lock.json').write_text(json.dumps(lock, indent=2) + '\n')
print('Pinned', len(a), 'artifacts in dependencies.lock.json')
