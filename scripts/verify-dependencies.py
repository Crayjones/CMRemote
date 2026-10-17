#!/usr/bin/env python3
"""Verify pinned artifacts and header contracts without installing dependencies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tarfile
import ssl
import subprocess
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
API = {
    'include/freerdp/freerdp.h': ['freerdp_context_new', 'freerdp_context_free', 'freerdp_connect',
        'freerdp_get_event_handles', 'freerdp_check_event_handles', 'freerdp_abort_connect_context',
        'freerdp_disconnect', 'freerdp_reconnect', 'VerifyCertificateEx', 'VerifyChangedCertificateEx'],
    'include/freerdp/input.h': ['freerdp_input_send_keyboard_event_ex', 'freerdp_input_send_unicode_keyboard_event',
        'freerdp_input_send_mouse_event'],
    'include/freerdp/client/cliprdr.h': ['CliprdrClientContext', 'MonitorReady', 'ClientCapabilities',
        'ServerFormatList', 'ClientFormatList', 'ClientFormatDataRequest', 'ServerFormatDataResponse',
        'ServerFormatDataRequest', 'ClientFormatDataResponse'],
    'include/freerdp/client/disp.h': ['DispClientContext', 'DisplayControlCaps', 'SendMonitorLayout'],
    'include/freerdp/gdi/gdi.h': ['gdi_init', 'gdi_resize', 'gdi_free', 'primary_buffer', 'stride'],
    'include/freerdp/update.h': ['BeginPaint', 'EndPaint', 'DesktopResize'],
    'include/freerdp/client.h': ['freerdp_client_load_channels'],
}

PERFORMANCE_API = {
    'sdl3': {
        'include/SDL3/SDL_render.h': ['SDL_UpdateTexture', 'SDL_RenderPresent', 'SDL_RenderReadPixels'],
        'LICENSE.txt': ['This notice may not be removed or altered from any source distribution.'],
    },
    'sdl3_ttf': {
        'CMakeLists.txt': ['set(SDL_REQUIRED_VERSION 3.2.6)', 'find_package(Freetype REQUIRED)',
                          'SDLTTF_HARFBUZZ', 'SDLTTF_PLUTOSVG', 'SDLTTF_VENDORED'],
        'LICENSE.txt': ['This notice may not be removed or altered from any source distribution.'],
    },
    'freetype_comparator': {
        'CMakeLists.txt': ['FT_DISABLE_ZLIB', 'FT_DISABLE_PNG', 'FT_DISABLE_BZIP2',
                          'FT_DISABLE_HARFBUZZ', 'FT_DISABLE_BROTLI'],
        'docs/FTL.TXT': ['The FreeType Project LICENSE', 'Redistribution'],
    },
    'openh264': {
        'codec/api/wels/codec_api.h': ['WelsCreateDecoder', 'WelsCreateSVCEncoder', 'WelsDestroyDecoder'],
        'codec/api/wels/codec_ver.h': ['OPENH264_MAJOR', 'OPENH264_MINOR'],
        'LICENSE': ['Redistribution and use in source and binary forms', 'Copyright (c) 2013, Cisco Systems'],
    },
}


def verify_performance_api(sources):
    for key, files in PERFORMANCE_API.items():
        for path, symbols in files.items():
            text = sources.get(key, {}).get(path)
            if text is None or any(symbol not in text for symbol in symbols):
                raise ValueError(key + ': missing source API or license contract in ' + path)


SSH_API = {
    'libvterm': {
        'LICENSE': ['The MIT License', 'Copyright (c) 2008 Paul Evans'],
        'include/vterm.h': ['VTERM_VERSION_MAJOR 0', 'VTERM_VERSION_MINOR 3',
            'vterm_input_write', 'vterm_output_set_callback', 'vterm_screen_get_cell',
            'vterm_keyboard_key', 'vterm_keyboard_unichar', 'vterm_keyboard_start_paste',
            'vterm_keyboard_end_paste', 'vterm_set_utf8', 'vterm_set_size',
            'sb_pushline', 'sb_popline', 'vterm_screen_enable_altscreen'],
    },
    'russh': {
        'Cargo.toml': ['version = "0.64.1"', 'license = "Apache-2.0"', 'rust-version = "1.89"'],
        'src/client/mod.rs': ['fn check_server_key', 'async { Ok(false) }',
            'pub async fn connect_stream', 'pub async fn authenticate_password',
            'pub async fn authenticate_publickey', 'pub async fn disconnect'],
        'src/client/encrypted.rs': ['self.common.auth_method = None;'],
        'src/channels/mod.rs': ['pub async fn request_pty', 'pub async fn request_shell',
            'pub async fn window_change', 'pub async fn data'],
        'src/keys/mod.rs': ['pub use ssh_key', 'pub fn load_secret_key'],
    },
}


def verify_ssh_api(sources):
    for key, files in SSH_API.items():
        for path, symbols in files.items():
            text = sources.get(key, {}).get(path)
            if text is None or any(symbol not in text for symbol in symbols):
                raise ValueError(key + ': missing SSH API or license contract in ' + path)


def cache_dir(platform):
    if platform not in ('macos', 'linux'):
        raise ValueError('platform must be macos or linux')
    return ROOT / '.dev' / platform / 'downloads'


def verify_bytes(data, expected):
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError('SHA-256 mismatch')


def download(url):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError('artifact URL must use HTTPS without credentials')
    request = urllib.request.Request(url, headers={'User-Agent': 'RemoteManager/0.1'})
    try:
        with urllib.request.urlopen(request, timeout=180) as response:
            if urllib.parse.urlsplit(response.geturl()).scheme != 'https':
                raise ValueError('artifact redirected outside HTTPS')
            return response.read()
    except urllib.error.URLError as error:
        # Apple's older Python TLS cannot negotiate with the libvterm host.
        if (not isinstance(error.reason, ssl.SSLError)
                or isinstance(error.reason, ssl.SSLCertVerificationError)
                or 'TLSV1_ALERT_PROTOCOL_VERSION' not in str(error.reason)):
            raise
        result = subprocess.run(['curl', '--fail', '--silent', '--show-error',
                                 '--proto', '=https', '--max-time', '180',
                                 '--user-agent', 'RemoteManager/0.1', url],
                                capture_output=True, check=True, timeout=190)
        return result.stdout


def fetch(artifact, cache):
    path = cache / artifact['file']
    if Path(artifact['file']).name != artifact['file'] or path.is_symlink():
        raise ValueError('artifact path must be a non-symlink basename')
    if not path.exists():
        data = download(artifact['url'])
        verify_bytes(data, artifact['sha256'])
        path.write_bytes(data)
    verify_bytes(path.read_bytes(), artifact['sha256'])
    return path


def verify_api(sources):
    for file, symbols in API.items():
        text = sources[file]
        for symbol in symbols:
            if symbol not in text:
                raise ValueError(file + ': missing ' + symbol)


def audit(cache, lock):
    with tarfile.open(cache / lock['artifacts']['freerdp']['file']) as archive:
        prefix = 'freerdp-' + lock['versions']['freerdp'] + '/'
        verify_api({file: archive.extractfile(prefix + file).read().decode() for file in API})
    for platform, arch in [('macos', 'clang_64'), ('linux-arm64', 'linux_gcc_arm64'), ('linux-amd64', 'linux_gcc_64')]:
        host = {'macos': 'mac_x64', 'linux-arm64': 'linux_arm64', 'linux-amd64': 'linux_x64'}[platform]
        tree = ET.parse(cache / lock['artifacts']['qt-metadata-' + platform]['file'])
        package = next(p for p in tree.findall('PackageUpdate') if p.findtext('Name') == 'qt.qt6.6120.' + arch)
        assert 'qtbase-' in package.findtext('DownloadableArchives')
        assert package.findtext('Version').startswith(lock['versions']['qt'] + '-')
        if platform == 'macos':
            assert 'Universal' in package.findtext('Description')
        assert host in lock['artifacts']['qt-metadata-' + platform]['url']
    assert lock['versions']['cxx'] == '1.0.202'
    with tarfile.open(cache / lock['artifacts']['cxx']['file']) as archive:
        manifest = archive.extractfile('cxx-1.0.202/Cargo.toml').read().decode()
        assert 'rust-version = "1.88"' in manifest
    assert tuple(map(int, lock['versions']['rust'].split('.'))) >= (1, 88, 0)
    roots = {'sdl3': 'SDL3-', 'sdl3_ttf': 'SDL3_ttf-', 'freetype_comparator': 'freetype-', 'openh264': 'openh264-'}
    sources = {}
    for key, files in PERFORMANCE_API.items():
        with tarfile.open(cache / lock['artifacts'][key]['file']) as archive:
            prefix = roots[key] + lock['versions'][key] + '/'
            sources[key] = {path: archive.extractfile(prefix + path).read().decode() for path in files}
    verify_performance_api(sources)
    sources = {}
    for key, files in SSH_API.items():
        with tarfile.open(cache / lock['artifacts'][key]['file']) as archive:
            prefix = key + '-' + lock['versions'][key] + '/'
            sources[key] = {path: archive.extractfile(prefix + path).read().decode() for path in files}
    verify_ssh_api(sources)
    from rust_notices import audit_vendored
    audit_vendored(lock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['macos', 'linux'], required=True)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    before = dict(os.environ)
    cache = cache_dir(args.platform)
    cache.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    required = ['freerdp', 'cxx', 'cxx-build', 'rust-manifest'] + list(PERFORMANCE_API) + list(SSH_API)
    required += [key for key in lock['artifacts'] if key.startswith('rust-notice-')]
    required += ['qt-metadata-' + p for p in ('macos', 'linux-arm64', 'linux-amd64')]
    for key in required:
        a = lock['artifacts'][key]
        if args.fetch:
            fetch(a, cache)
        else:
            verify_bytes((cache / a['file']).read_bytes(), a['sha256'])
    audit(cache, lock)
    assert before == dict(os.environ)
    print('PASS: pinned source checksums, FreeRDP/russh/libvterm APIs, vendored vnc-rs baseline/notices, license artifacts, minimal Qt packages, cxx toolchain, containment')


if __name__ == '__main__':
    main()
