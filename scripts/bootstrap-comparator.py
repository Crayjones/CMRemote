#!/usr/bin/env python3
"""Build test-only SDL-FreeRDP in a separate project prefix; leave app SDK unchanged."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def options(key, prefix, app_prefix):
    common = ['-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
              '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=ON',
              '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DCMAKE_INSTALL_RPATH=' + str(prefix / 'lib')]
    if key == 'sdl3':
        return common + ['-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF',
                         '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF', '-DSDL_INSTALL=ON',
                         '-DSDL_INSTALL_CPACK=OFF']
    if key == 'freetype_comparator':
        return common + ['-DFT_DISABLE_' + name + '=ON' for name in
                         ('ZLIB', 'PNG', 'BZIP2', 'HARFBUZZ', 'BROTLI')]
    if key == 'sdl3_ttf':
        return common + ['-DSDLTTF_VENDORED=OFF', '-DSDLTTF_HARFBUZZ=OFF', '-DSDLTTF_PLUTOSVG=OFF',
                         '-DSDLTTF_SAMPLES=OFF', '-DSDLTTF_INSTALL=ON', '-DSDLTTF_STRICT=ON',
                         '-DSDLTTF_INSTALL_CPACK=OFF']
    if key != 'freerdp':
        raise ValueError('Unknown comparator dependency')
    disabled = ('WITH_SERVER', 'WITH_SAMPLE', 'BUILD_TESTING', 'WITH_X11', 'WITH_WAYLAND',
                'WITH_FFMPEG', 'WITH_SWSCALE', 'WITH_OPENH264', 'WITH_CUPS', 'WITH_PULSE', 'WITH_ALSA',
                'WITH_OSS', 'WITH_SNDIO', 'WITH_MACAUDIO', 'WITH_PCSC', 'WITH_PKCS11', 'WITH_FUSE',
                'WITH_KRB5', 'WITH_GSSAPI', 'WITH_WINPR_JSON', 'WITH_AAD', 'WITH_MANPAGES', 'WITH_CCACHE',
                'WITH_LIBUSB', 'WITH_WEBVIEW', 'WITH_SMARTCARD_EMULATE', 'CHANNEL_URBDRC',
                'CHANNEL_SMARTCARD', 'CHANNEL_RDPDR', 'CHANNEL_RDPSND', 'CHANNEL_AUDIN',
                'WITH_CLIENT_SDL2', 'WITH_CLIENT_MAC', 'WITH_CLIENT_X11', 'WITH_CLIENT_WAYLAND')
    return common + ['-D' + name + '=OFF' for name in disabled] + [
        '-DWITH_CLIENT=ON', '-DWITH_CLIENT_COMMON=ON', '-DWITH_CLIENT_CHANNELS=ON',
        '-DWITH_CLIENT_SDL=ON', '-DWITH_CLIENT_SDL3=ON', '-DWITH_SDL_LINK_SHARED=ON',
        '-DOPENSSL_ROOT_DIR=' + str(app_prefix), '-DZLIB_ROOT=' + str(app_prefix),
        '-DCMAKE_INSTALL_RPATH=' + str(prefix / 'lib') + ';' + str(app_prefix / 'lib'),
        '-DWITH_ABSOLUTE_PLUGIN_LOAD_PATHS=OFF']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['macos', 'linux'], required=True)
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    if (args.platform == 'macos') != (platform.system() == 'Darwin') or platform.system() not in ('Darwin', 'Linux'):
        parser.error('Comparator bootstrap must run on its native platform')
    if args.jobs < 1 or args.jobs > 64: parser.error('jobs must be 1..64')
    bootstrap = load('comparator_bootstrap', ROOT / 'scripts/bootstrap.py')
    env_module = load('comparator_env', ROOT / 'scripts/env.py')
    env = env_module.environment()
    app_prefix, qt = Path(env['RM_NATIVE_PREFIX']), Path(env['RM_QT_PREFIX'])
    dev = ROOT / '.dev' / args.platform
    prefix = dev / 'comparator/prefix'
    env = dict(env, LD_LIBRARY_PATH=os.pathsep.join([str(prefix / 'lib'), str(app_prefix / 'lib'),
        str(qt / 'lib'), str(qt), env.get('LD_LIBRARY_PATH', '')]))
    source = dev / 'comparator/src'
    builds = Path(env['RM_BUILD_DIR']) / 'comparator'
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    cmake = shutil.which('cmake', path=env['PATH'])
    if not cmake or not (app_prefix / 'include/freerdp3/freerdp/freerdp.h').is_file():
        parser.error('Run the contained project bootstrap first')
    (dev / 'downloads').mkdir(parents=True, exist_ok=True)
    names = {'sdl3': 'SDL3-', 'sdl3_ttf': 'SDL3_ttf-', 'freetype_comparator': 'freetype-', 'freerdp': 'freerdp-'}
    for key in ('sdl3', 'freetype_comparator', 'sdl3_ttf', 'freerdp'):
        archive = bootstrap.audit.fetch(lock['artifacts'][key], dev / 'downloads')
        src = source / (names[key] + lock['versions'][key])
        if not src.is_dir(): bootstrap.unpack(archive, source)
        args_list = options(key, prefix, app_prefix)
        if key == 'freerdp':
            load('comparator_probe', ROOT / 'scripts/comparator-probe.py').instrument(src)
            args_list += ['-DQt6_DIR=' + str(qt / 'lib/cmake/Qt6'),
                          '-DRM_COMPARATOR_RUNTIME_PATHS=' + ';'.join(str(path) for path in
                              (prefix / 'lib', app_prefix / 'lib', qt / 'lib'))]
        if key == 'freerdp' and args.platform == 'linux': args_list += ['-DWITH_UNICODE_BUILTIN=ON']
        bootstrap.run(cmake, '-S', src, '-B', builds / key, '-G', 'Ninja', *args_list, env=env)
        bootstrap.run(cmake, '--build', builds / key, '--parallel', str(args.jobs), env=env)
        bootstrap.run(cmake, '--install', builds / key, env=env)
    bootstrap.run(prefix / 'bin/sdl-freerdp', '/version', env=env)
    notices = prefix / 'share/remotemanager-comparator-notices'
    notices.mkdir(parents=True, exist_ok=True)
    for name, path in {
        'SDL3-LICENSE.txt': source / ('SDL3-' + lock['versions']['sdl3']) / 'LICENSE.txt',
        'SDL3_ttf-LICENSE.txt': source / ('SDL3_ttf-' + lock['versions']['sdl3_ttf']) / 'LICENSE.txt',
        'FreeType-FTL.txt': source / ('freetype-' + lock['versions']['freetype_comparator']) / 'docs/FTL.TXT',
        'FreeType-LICENSE.txt': source / ('freetype-' + lock['versions']['freetype_comparator']) / 'LICENSE.TXT',
        'FreeRDP-LICENSE.txt': source / ('freerdp-' + lock['versions']['freerdp']) / 'LICENSE',
    }.items(): shutil.copyfile(path, notices / name)
    print('Test-only comparator:', prefix / 'bin/sdl-freerdp')


if __name__ == '__main__':
    main()
