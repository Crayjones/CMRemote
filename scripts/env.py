#!/usr/bin/env python3
"""Launch one command in a project-local environment. Never source this file."""
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]


def windows_environment(arch):
    """Linux-container cross build for Windows x86_64; tool layout from scripts/bootstrap-windows.py."""
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    versions = lock['versions']
    sdk = ROOT / '.dev/windows'
    build = ROOT / 'build/windows'
    cmake_bin = sdk / 'tools/cmake' / ('cmake-' + versions['cmake'] + '-linux-' + ('aarch64' if arch == 'arm64' else 'x86_64')) / 'bin'
    mingw = sdk / ('llvm-mingw-' + versions['llvm_mingw'] + '-ucrt-ubuntu-22.04-' + ('aarch64' if arch == 'arm64' else 'x86_64'))
    qt, host_qt, native = sdk / 'qt', sdk / 'qt-host', sdk / 'prefix'
    if not (cmake_bin / 'cmake').is_file() or not (mingw / 'bin').is_dir() or not (qt / 'lib/cmake/Qt6').is_dir():
        raise SystemExit('Windows cross SDK missing. Run: python3 scripts/docker.py windows python3 scripts/bootstrap-windows.py')
    env = dict(os.environ)
    target = 'x86_64-pc-windows-gnullvm'
    env.update(CARGO_HOME=str(sdk / 'cargo'), RUSTUP_HOME=str(sdk / 'rustup'),
               CARGO_TARGET_DIR=str(build / 'cargo'), CARGO_BUILD_TARGET=target,
               RM_BUILD_DIR=str(build), RM_PLATFORM='windows', RM_TARGET_ARCH='amd64',
               RM_QT_PREFIX=str(qt), RM_QT_HOST_PATH=str(host_qt), RM_NATIVE_PREFIX=str(native),
               RM_LLVM_MINGW=str(mingw), RM_FIND_ROOTS=str(qt) + ':' + str(native),
               CMAKE_TOOLCHAIN_FILE=str(ROOT / 'cmake/windows-llvm-mingw.cmake'),
               CMAKE_PREFIX_PATH=str(qt) + os.pathsep + str(native),
               XDG_CACHE_HOME=str(sdk / 'cache'), PIP_CACHE_DIR=str(sdk / 'cache/pip'), PYTHONDONTWRITEBYTECODE='1')
    # cc-rs and Cargo use the same pinned clang for C/C++ in Rust crates (ring, cxx).
    key = target.replace('-', '_')
    env.update({'CC_' + key: 'x86_64-w64-mingw32-clang', 'CXX_' + key: 'x86_64-w64-mingw32-clang++',
                'AR_' + key: 'llvm-ar', 'CARGO_TARGET_' + key.upper() + '_LINKER': 'x86_64-w64-mingw32-clang'})
    env['PATH'] = os.pathsep.join([str(mingw / 'bin'), str(sdk / 'cargo/bin'), str(cmake_bin), str(sdk / 'tools/bin'), env['PATH']])
    # Host Qt tools need their libraries; Qt's ICU archive unpacks at the prefix root.
    env['LD_LIBRARY_PATH'] = os.pathsep.join([str(host_qt / 'lib'), str(host_qt), env.get('LD_LIBRARY_PATH', '')])
    for path in (sdk / 'cache', build): path.mkdir(parents=True, exist_ok=True)
    return env


def environment():
    host = 'macos' if platform.system() == 'Darwin' else 'linux'
    arch = 'arm64' if platform.machine() in ('arm64', 'aarch64') else 'amd64'
    if os.environ.get('RM_TARGET_PLATFORM') == 'windows':
        if host != 'linux': raise SystemExit('Windows builds run in the Linux cross container: scripts/docker.py windows ...')
        return windows_environment(arch)
    local = ROOT / '.dev' / host
    sdk = Path(os.environ.get('RM_SDK_ROOT', str(local)))
    build = ROOT / 'build' / host
    if host == 'linux': build /= arch
    # macOS can cross-build another CPU from its separate native SDK; Qt is universal.
    target = os.environ.get('RM_TARGET_ARCH', arch)
    if target not in ('arm64', 'amd64'): raise SystemExit('RM_TARGET_ARCH must be arm64 or amd64')
    cross = target != arch
    if cross and host != 'macos': raise SystemExit('RM_TARGET_ARCH cross builds are macOS-only; use the matching Linux container')
    native = sdk / target / 'prefix' if cross else sdk / 'prefix'
    if cross: build /= target
    qt_arch = 'macos' if host == 'macos' else ('gcc_arm64' if arch == 'arm64' else 'gcc_64')
    qt = sdk / 'qt'
    versions = json.loads((ROOT / 'dependencies.lock.json').read_text())['versions']
    cmake_host = 'macos-universal' if host == 'macos' else 'linux-' + ('aarch64' if arch == 'arm64' else 'x86_64')
    cmake_bin = sdk / 'tools/cmake' / ('cmake-' + versions['cmake'] + '-' + cmake_host) / ('CMake.app/Contents/bin' if host == 'macos' else 'bin')
    cmake = cmake_bin / 'cmake'
    ninja = sdk / 'tools/bin/ninja'
    if not qt.exists() or not cmake.is_file() or not os.access(cmake, os.X_OK):
        raise SystemExit('Project SDK missing pinned CMake ' + versions['cmake'] + ' or Qt. Run scripts/bootstrap-macos.sh or scripts/linux.sh image.')
    try:
        ninja_version = subprocess.check_output([str(ninja), '--version'], text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError):
        ninja_version = None
    if ninja_version != versions['ninja']:
        raise SystemExit('Project SDK requires pinned Ninja ' + versions['ninja'] + '. Run scripts/bootstrap-macos.sh or scripts/linux.sh image.')
    env = dict(os.environ)
    env.update(CARGO_HOME=str(local / 'cargo'), RUSTUP_HOME=str(sdk / 'rustup'),
               CARGO_TARGET_DIR=str(build / 'cargo'), RM_BUILD_DIR=str(build), RM_PLATFORM=host,
               RM_QT_PREFIX=str(qt), RM_NATIVE_PREFIX=str(native), RM_TARGET_ARCH=target,
               CMAKE_PREFIX_PATH=str(qt) + os.pathsep + str(native),
               XDG_CACHE_HOME=str(local / 'cache'), PIP_CACHE_DIR=str(local / 'cache/pip'),
               QT_LOGGING_RULES='qt.qpa.*=false', PYTHONDONTWRITEBYTECODE='1')
    env['PATH'] = os.pathsep.join([str(sdk / 'cargo/bin'), str(cmake_bin), str(sdk / 'tools/bin'), str(qt / 'bin'), env['PATH']])
    if cross:
        machine = 'x86_64' if target == 'amd64' else 'arm64'
        env.update(CMAKE_OSX_ARCHITECTURES=machine, CARGO_BUILD_TARGET=machine + '-apple-darwin')
    else:
        env.pop('CMAKE_OSX_ARCHITECTURES', None); env.pop('CARGO_BUILD_TARGET', None)
    env['LD_LIBRARY_PATH'] = os.pathsep.join([str(native / 'lib'), str(qt / 'lib'), str(qt), env.get('LD_LIBRARY_PATH', '')])
    # Prefer framework-relative Qt plugins; all selected paths are project-managed.
    env['QT_PLUGIN_PATH'] = str(qt / 'plugins')
    for p in [local / 'cargo', local / 'cache', build]: p.mkdir(parents=True, exist_ok=True)
    return env


if __name__ == '__main__':
    if len(sys.argv) < 2: raise SystemExit('Usage: python3 scripts/env.py COMMAND [ARG ...]')
    raise SystemExit(subprocess.call(sys.argv[1:], cwd=ROOT, env=environment()))
