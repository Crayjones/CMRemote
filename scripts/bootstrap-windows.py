#!/usr/bin/env python3
"""Cross SDK for Windows x86_64 in .dev/windows, built inside a Linux container. Run via scripts/windows.sh."""
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
spec = importlib.util.spec_from_file_location('bootstrap', ROOT / 'scripts/bootstrap.py')
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)
run = bootstrap.run
TRIPLE = 'x86_64-w64-mingw32'
RUST_TARGET = 'x86_64-pc-windows-gnullvm'
# Import libraries and DLLs each dependency must install; checked before any reuse.
OUTPUTS = {
    'openssl': ['lib/libssl.dll.a', 'lib/libcrypto.dll.a', 'bin/libssl-3-x64.dll', 'bin/libcrypto-3-x64.dll'],
    'zlib': ['include/zlib.h'],
    'openh264': ['include/wels/codec_api.h', 'bin/libopenh264.dll', 'lib/libopenh264.dll.a',
                 'share/licenses/openh264/LICENSE'],
    'freerdp': ['include/freerdp3/freerdp/freerdp.h', 'lib/cmake/FreeRDP3/FreeRDPConfig.cmake',
                'bin/libfreerdp3.dll', 'bin/libfreerdp-client3.dll', 'bin/libwinpr3.dll'],
}


def llvm_mingw_dir(dev, lock, arch):
    return dev / ('llvm-mingw-' + lock['versions']['llvm_mingw'] + '-ucrt-ubuntu-22.04-' + ('aarch64' if arch == 'arm64' else 'x86_64'))


def freerdp_options(prefix):
    # Same feature contract as macOS/Linux: client library only, no device redirection or audio.
    options = [option for option in bootstrap.freerdp_options(prefix, 'linux')
               if not option.startswith(('-DOPENH264_LIBRARY=', '-DWITH_UNICODE_BUILTIN='))]
    return options + ['-DOPENH264_LIBRARY=' + str(prefix / 'lib/libopenh264.dll.a'),
                      '-DWITH_URIPARSER=OFF', '-DWITH_JSON_DISABLED=ON', '-DWITH_WIN_CONSOLE=OFF',
                      # llvm-mingw ships unwind.h, but WinPR's unwinder needs dlfcn; Windows has its own.
                      '-DUSE_UNWIND=OFF', '-DUSE_EXECINFO=OFF',
                      # UCRT has no C11 <threads.h>; WinPR then uses its Windows TLS path.
                      # Clang 22 rejects DWORD*/UINT32* mixing that is size-identical on LLP64 Windows.
                      '-DCMAKE_C_FLAGS=-D__STDC_NO_THREADS__=1 -Wno-error=incompatible-pointer-types']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    args = p.parse_args()
    if platform.system() != 'Linux':
        raise SystemExit('The Windows cross SDK builds in a Linux container: scripts/windows.sh bootstrap')
    arch = 'arm64' if platform.machine() in ('arm64', 'aarch64') else 'amd64'
    dev = ROOT / '.dev/windows'
    cache = dev / 'downloads'
    cache.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    a, versions = lock['artifacts'], lock['versions']
    def artifact(key):
        return bootstrap.audit.fetch(a[key], cache)
    bootstrap.fetch_rust_notices(a, artifact)
    cmake = bootstrap.install_build_tools(dev / 'tools', 'linux-' + arch, versions, artifact)
    # Host Qt supplies moc/rcc/uic/qsb; target Qt supplies the Windows libraries and plugins.
    host_qt = dev / 'qt-host'
    bootstrap.install_qt_modules(host_qt, 'linux-' + arch, a, artifact)
    bootstrap.validate_shadertools(host_qt, ROOT / 'build/windows/deps')
    qt = dev / 'qt'
    if not (qt / 'lib/cmake/Qt6/Qt6Config.cmake').is_file():
        bootstrap.unpack(artifact('qtbase-windows'), qt)
    if not (qt / 'lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake').is_file():
        bootstrap.unpack(artifact('qtshadertools-windows'), qt)
    mingw = llvm_mingw_dir(dev, lock, arch)
    if not (mingw / 'bin' / (TRIPLE + '-clang')).exists():
        bootstrap.unpack(artifact('llvm-mingw-linux-' + arch), dev)
    env = dict(os.environ, CARGO_HOME=str(dev / 'cargo'), RUSTUP_HOME=str(dev / 'rustup'),
               CARGO_TARGET_DIR=str(ROOT / 'build/windows/cargo'), XDG_CACHE_HOME=str(dev / 'cache'))
    env['PATH'] = os.pathsep.join([str(mingw / 'bin'), str(dev / 'cargo/bin'), str(cmake.parent),
                                   str(dev / 'tools/bin'), env['PATH']])
    ceiling = [str(ROOT)] + ([env['GIT_CEILING_DIRECTORIES']] if env.get('GIT_CEILING_DIRECTORIES') else [])
    env['GIT_CEILING_DIRECTORIES'] = os.pathsep.join(ceiling)
    if not (dev / 'cargo/bin/rustup').exists():
        installer = artifact('rustup-linux-' + arch)
        installer.chmod(0o755)
        run(installer, '-y', '--no-modify-path', '--profile', 'minimal', '--default-toolchain', versions['rust'], env=env)
    run(dev / 'cargo/bin/rustup', 'target', 'add', RUST_TARGET, env=env)
    prefix = dev / 'prefix'
    source = dev / 'src'
    builds = ROOT / 'build/windows/deps'
    toolchain = ROOT / 'cmake/windows-llvm-mingw.cmake'
    cross = dict(env, RM_LLVM_MINGW=str(mingw), RM_FIND_ROOTS=str(prefix))
    common = ['-G', 'Ninja', '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain), '-DCMAKE_BUILD_TYPE=Release',
              '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
              '-DCMAKE_PREFIX_PATH=' + str(prefix)]
    for key in ('openssl', 'zlib', 'openh264', 'freerdp', 'libvterm'):
        version = versions[key]
        src = source / (key + '-' + version)
        if not src.exists():
            bootstrap.unpack(artifact(key), source)
        contract = {'version': version, 'source': a[key]['sha256'], 'target': TRIPLE, 'toolchain': versions['llvm_mingw']}
        if key == 'libvterm':
            files = bootstrap.LIBVTERM_FILES
            if not bootstrap.dependency_current(prefix, key, contract, files):
                run(cmake, '-S', ROOT / 'cmake/libvterm', '-B', builds / key, *common,
                    '-DRM_LIBVTERM_SOURCE=' + str(src), env=cross)
                run(cmake, '--build', builds / key, '--parallel', str(args.jobs), env=cross)
                run(cmake, '--install', builds / key, env=cross)
                record(prefix, key, contract, files)
            continue
        if key == 'freerdp':
            bootstrap.patch_freerdp(src)
            contract['options'] = freerdp_options(prefix)
            contract['loader_patch'] = bootstrap.OPENH264_LOADER_PATCH
        files = OUTPUTS[key]
        if bootstrap.dependency_current(prefix, key, contract, files):
            continue
        if key == 'openssl':
            build = builds / key
            shutil.rmtree(build, ignore_errors=True); build.mkdir(parents=True)
            run('perl', src / 'Configure', 'mingw64', 'no-tests', 'shared', '--cross-compile-prefix=' + TRIPLE + '-',
                '--prefix=' + str(prefix), '--openssldir=' + str(prefix / 'ssl'), '--libdir=lib',
                'CC=clang', 'CXX=clang++', 'AR=llvm-ar', 'RANLIB=llvm-ranlib', 'RC=windres', cwd=build, env=cross)
            run('make', '-j' + str(args.jobs), cwd=build, env=cross)
            run('make', 'install_sw', cwd=build, env=cross)
        elif key == 'openh264':
            build = builds / key
            shutil.rmtree(build, ignore_errors=True); build.mkdir(parents=True)
            command = ['make', '-f', src / 'Makefile', 'OS=mingw_nt', 'ARCH=x86_64', 'USE_ASM=No', 'BUILDTYPE=Release',
                       'PREFIX=' + str(prefix), 'CC=' + TRIPLE + '-clang', 'CXX=' + TRIPLE + '-clang++',
                       'AR=llvm-ar']
            run(*command, '-j' + str(args.jobs), 'libraries', cwd=build, env=cross)
            run(*command, 'install-shared', cwd=build, env=cross)
            notice = prefix / 'share/licenses/openh264'; notice.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src / 'LICENSE', notice / 'LICENSE')
        else:
            options = contract.get('options', [])
            run(cmake, '-S', src, '-B', builds / key, *common, *options, env=cross)
            run(cmake, '--build', builds / key, '--parallel', str(args.jobs), env=cross)
            run(cmake, '--install', builds / key, env=cross)
        record(prefix, key, contract, files)
    print('Ready:', dev, '\nNative SDK:', prefix, '\nQt (target):', qt, '\nQt (host):', host_qt, '\nToolchain:', mingw)


def record(prefix, key, contract, files):
    missing = [name for name in files if not (prefix / name).is_file()]
    if missing: raise RuntimeError(key + ' did not install: ' + ', '.join(missing))
    bootstrap.record_dependency(prefix, key, contract, files)


if __name__ == '__main__':
    main()
