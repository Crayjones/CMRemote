#!/usr/bin/env python3
"""Install removable tools and libraries into .dev/<platform>; never change HOME."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import zipfile
import tempfile
import sys
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('audit', ROOT / 'scripts/verify-dependencies.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def run(*args, cwd=None, env=None):
    subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)


def unpack(path, dest):
    dest.mkdir(parents=True, exist_ok=True)
    if path.suffix == '.7z':
        import py7zr
        with py7zr.SevenZipFile(path) as archive:
            if any(n.startswith('/') or '..' in Path(n).parts for n in archive.getnames()):
                raise ValueError('unsafe archive path')
            archive.extractall(dest)
    elif path.suffix == '.zip':
        with zipfile.ZipFile(path) as archive:
            if any(n.startswith('/') or '..' in Path(n).parts for n in archive.namelist()):
                raise ValueError('unsafe archive path')
            archive.extractall(dest)
    else:
        with tarfile.open(path) as archive:
            for member in archive.getmembers():
                if member.name.startswith('/') or '..' in Path(member.name).parts:
                    raise ValueError('unsafe archive path')
                if member.issym() or member.islnk():
                    target = (dest / member.name).parent / member.linkname
                    if not str(target.resolve()).startswith(str(dest.resolve()) + os.sep):
                        raise ValueError('unsafe archive symlink')
            archive.extractall(dest)


def fetch_rust_notices(artifacts, artifact):
    # Packaging audits Rust notices offline; upstream-only license files must be cached here.
    for key in artifacts:
        if key.startswith('rust-notice-'):
            artifact(key)


def install_build_tools(tools, target, versions, artifact):
    cmake_host = {'macos': 'macos-universal', 'linux-arm64': 'linux-aarch64',
                  'linux-amd64': 'linux-x86_64'}[target]
    cmake = tools / 'cmake' / ('cmake-' + versions['cmake'] + '-' + cmake_host) / ('CMake.app/Contents/bin/cmake' if target == 'macos' else 'bin/cmake')
    if not cmake.is_file() or not os.access(cmake, os.X_OK):
        unpack(artifact('cmake-' + target), tools / 'cmake')
    if not cmake.is_file() or not os.access(cmake, os.X_OK):
        raise RuntimeError('Pinned CMake ' + versions['cmake'] + ' missing after installation: ' + str(cmake))
    ninja = tools / 'bin/ninja'
    def ninja_current():
        try:
            return subprocess.check_output([str(ninja), '--version'], text=True, stderr=subprocess.STDOUT).strip() == versions['ninja']
        except (OSError, subprocess.CalledProcessError):
            return False
    if not ninja_current():
        unpack(artifact('ninja-' + target), tools / 'bin')
        ninja.chmod(0o755)
        if not ninja_current():
            raise RuntimeError('Installed Ninja does not match pinned ' + versions['ninja'])
    return cmake


def qt_module_installed(qt, module):
    if module == 'qtbase':
        return (qt / 'lib/cmake/Qt6/Qt6Config.cmake').is_file()
    if module == 'qtwayland':
        return (qt / 'lib/cmake/Qt6WaylandClient/Qt6WaylandClientConfig.cmake').is_file()
    if module == 'icu':
        return any((qt / 'lib').glob('libicuuc.so*'))
    if module == 'qtshadertools':
        return all((qt / path).is_file() for path in (
            'lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake',
            'lib/cmake/Qt6ShaderToolsTools/Qt6ShaderToolsToolsConfig.cmake',
            'lib/cmake/Qt6ShaderToolsTools/Qt6ShaderToolsMacros.cmake')) and os.access(qt / 'bin/qsb', os.X_OK)
    raise ValueError('Unknown Qt module')


def install_qt_modules(qt, target, artifacts, artifact):
    for module in ('qtbase', 'qtwayland', 'icu', 'qtshadertools'):
        key = module + '-' + target
        if key in artifacts and not qt_module_installed(qt, module):
            unpack(artifact(key), qt)


def validate_shadertools(qt, workspace):
    if not qt_module_installed(qt, 'qtshadertools'):
        raise RuntimeError('Project Qt ShaderTools installation is incomplete')
    qsb = qt / 'bin/qsb'
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = os.pathsep.join([str(qt / 'lib'), str(qt), env.get('LD_LIBRARY_PATH', '')])
    try:
        version = subprocess.check_output([str(qsb), '--version'], text=True, stderr=subprocess.STDOUT, env=env)
    except subprocess.CalledProcessError as error:
        raise RuntimeError('Project qsb could not run: ' + error.output.strip()) from error
    if '6.12.0' not in version:
        raise RuntimeError('Project qsb does not match pinned Qt 6.12.0')
    workspace.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='shader-check-', dir=workspace) as directory:
        for name in ('desktop.vert', 'desktop.frag'):
            output = Path(directory) / (name + '.qsb')
            run(qsb, '--glsl', '100 es,120,150', '--hlsl', '50', '--msl', '12',
                '-o', output, ROOT / 'app/shaders' / name, env=env)
            if not output.is_file() or output.stat().st_size == 0:
                raise RuntimeError('Project qsb produced no shader package')
            subprocess.run([str(qsb), '-d', str(output)], check=True, stdout=subprocess.DEVNULL, env=env)


def shared_library(prefix, name, host):
    if host not in ('macos', 'linux'): raise ValueError('Unsupported native library platform')
    return prefix / 'lib' / ('lib' + name + ('.dylib' if host == 'macos' else '.so'))


def freerdp_options(prefix, host):
    # Remote audio playback (rdpsnd) needs rdpdr registered plus a native backend.
    # macOS uses CoreAudio; Linux keeps audio off until a PulseAudio runtime is packaged.
    # rdpdr carries no devices: drive/printer/serial/parallel/smartcard stay off.
    audio = host == 'macos'
    # Pinned tarballs carry no git history; always use their hardcoded version, never the enclosing checkout.
    off = ['USE_VERSION_FROM_GIT_TAG', 'USE_GIT_FOR_REVISION',
        'WITH_CLIENT', 'WITH_SERVER', 'WITH_SAMPLE', 'BUILD_TESTING', 'WITH_X11', 'WITH_WAYLAND',
        'WITH_FFMPEG', 'WITH_VIDEO_FFMPEG', 'WITH_DSP_FFMPEG', 'WITH_FFMPEG_HWACCEL', 'WITH_SWSCALE',
        'WITH_MEDIA_FOUNDATION', 'WITH_MEDIACODEC',
        # FreeRDP turns Opus on whenever pkg-config finds one, which bypasses CMAKE_IGNORE_PREFIX_PATH
        # and links an unpinned host libopus (Homebrew's breaks the <opus/opus.h> include) into the SDK.
        'WITH_OPUS',
        'WITH_CUPS', 'WITH_PULSE', 'WITH_ALSA', 'WITH_OSS', 'WITH_SNDIO',
        'WITH_PCSC', 'WITH_PKCS11', 'WITH_FUSE', 'WITH_KRB5', 'WITH_GSSAPI', 'WITH_WINPR_JSON',
        'WITH_AAD', 'WITH_MANPAGES', 'WITH_CCACHE', 'WITH_LIBUSB', 'WITH_WEBVIEW',
        'WITH_SMARTCARD_EMULATE', 'CHANNEL_URBDRC', 'CHANNEL_SMARTCARD',
        'CHANNEL_AUDIN', 'CHANNEL_DRIVE', 'CHANNEL_PRINTER', 'CHANNEL_SERIAL', 'CHANNEL_PARALLEL',
        'WITH_ABSOLUTE_PLUGIN_LOAD_PATHS']
    audio_options = ['WITH_MACAUDIO', 'CHANNEL_RDPDR', 'CHANNEL_RDPSND']
    if not audio: off += audio_options
    options = ['-D' + option + '=OFF' for option in off]
    if audio: options += ['-D' + option + '=ON' for option in audio_options]
    options += ['-DWITH_CLIENT_COMMON=ON', '-DWITH_CLIENT_CHANNELS=ON', '-DBUILD_SHARED_LIBS=ON',
                '-DOPENSSL_ROOT_DIR=' + str(prefix), '-DWITH_OPENH264=ON',
                # The Cisco-provided decoder is downloaded by the app and loaded at runtime, never linked.
                '-DWITH_OPENH264_LOADING=ON',
                '-DOPENH264_INCLUDE_DIR=' + str(prefix / 'include'),
                '-DOPENH264_LIBRARY=' + str(shared_library(prefix, 'openh264', host))]
    if host == 'linux': options += ['-DWITH_UNICODE_BUILTIN=ON']
    return options


def dependency_files(prefix, files):
    records = {}
    for name in files:
        path = prefix / name
        if not path.resolve().is_relative_to(prefix.resolve()):
            raise ValueError('Dependency file escapes its prefix')
        records[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return records


def dependency_current(prefix, name, contract, files):
    try:
        manifest = json.loads((prefix / ('.rm-' + name + '.json')).read_text())
        return manifest == {'schema': 1, 'contract': contract, 'files': dependency_files(prefix, files)}
    except (OSError, ValueError):
        return False


def record_dependency(prefix, name, contract, files):
    manifest = {'schema': 1, 'contract': contract, 'files': dependency_files(prefix, files)}
    (prefix / ('.rm-' + name + '.json')).write_text(json.dumps(manifest, indent=2) + '\n')


# FreeRDP's runtime loader searches fixed library names. CMRemote instead supplies the exact
# verified Cisco binary it downloaded through freerdp_openh264_set_library; with none, H.264 stays off.
OPENH264_LOADER_ORIGINAL = """	for (size_t i = 0; i < ARRAYSIZE(openh264_library_names); i++)
	{
		const char* current = openh264_library_names[i];
		success = openh264_load_functionpointers(h264, current);

		if (success)
			break;
	}
"""
OPENH264_LOADER_PATCHED = """	/* CMRemote: load only the application-supplied, separately downloaded binary. */
	{
		const char* path = atomic_load(&openh264_library_path);
		if (path)
		{
			success = openh264_load_functionpointers(h264, path);
#if defined(_WIN32)
			/* The application preloads the DLL by wide path; this name finds that module. */
			for (size_t i = 0; !success && i < ARRAYSIZE(openh264_library_names); i++)
				success = openh264_load_functionpointers(h264, openh264_library_names[i]);
#endif
		}
	}
"""
OPENH264_SETTER_ANCHOR = "static void openh264_trace_callback("
OPENH264_SETTER = """#if defined(WITH_OPENH264_LOADING)
/* CMRemote: replaced strings are never freed, because a loader may still be reading one. */
static _Atomic(char*) openh264_library_path = nullptr;

FREERDP_API void freerdp_openh264_set_library(const char* path);
FREERDP_API void freerdp_openh264_set_library(const char* path)
{
	char* copy = (path && *path) ? _strdup(path) : nullptr;
	if (path && *path && !copy)
		return;
	(void)atomic_exchange(&openh264_library_path, copy);
}
#endif

"""
OPENH264_LOADER_PATCH = hashlib.sha256(
    (OPENH264_LOADER_ORIGINAL + OPENH264_LOADER_PATCHED + OPENH264_SETTER).encode()).hexdigest()


def patch_freerdp(src):
    path = src / 'libfreerdp/codec/h264_openh264.c'
    text = path.read_text()
    if OPENH264_LOADER_PATCHED in text and OPENH264_SETTER in text: return
    original = path.with_suffix('.c.orig')
    if original.exists(): text = original.read_text()
    else: original.write_text(text)
    if text.count(OPENH264_LOADER_ORIGINAL) != 1 or text.count(OPENH264_SETTER_ANCHOR) != 1:
        raise RuntimeError('FreeRDP OpenH264 loader differs from the pinned source')
    text = text.replace(OPENH264_LOADER_ORIGINAL, OPENH264_LOADER_PATCHED)
    text = text.replace(OPENH264_SETTER_ANCHOR, OPENH264_SETTER + OPENH264_SETTER_ANCHOR)
    text = text.replace('#include <winpr/winpr.h>\n', '#include <stdatomic.h>\n#include <winpr/winpr.h>\n#include <winpr/string.h>\n', 1)
    path.write_text(text)


def validate_freerdp_sdk(prefix, build, options):
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if '=' in line and ':' in line and not line.startswith(('//', '#')):
            name, value = line.split('=', 1); cache[name.split(':', 1)[0]] = value
    for option in options:
        name, value = option.removeprefix('-D').split('=', 1)
        if cache.get(name) != value:
            raise RuntimeError('FreeRDP configured option mismatch: ' + name)
    config = (prefix / 'include/freerdp3/freerdp/config.h').read_text().splitlines()
    for name in ('WITH_GFX_H264', 'WITH_OPENH264', 'WITH_OPENH264_LOADING'):
        if '#define ' + name not in config:
            raise RuntimeError('Installed FreeRDP missing compiled decoder capability: ' + name)
    for name in ('WITH_VIDEO_FFMPEG', 'WITH_DSP_FFMPEG', 'WITH_SWSCALE',
                 'WITH_MEDIA_FOUNDATION', 'WITH_MEDIACODEC', 'WITH_FFMPEG_HWACCEL'):
        if any(line.startswith('#define ' + name) for line in config):
            raise RuntimeError('Installed FreeRDP enabled an excluded decoder path: ' + name)


def install_openh264(source, build, prefix, jobs, env, machine=None):
    build.mkdir(parents=True, exist_ok=True)
    # The full library exports both encoder and decoder entry points used by FreeRDP.
    # Disable assembly to avoid an unpinned NASM prerequisite on x86_64.
    command = ['make', '-f', source / 'Makefile', 'PREFIX=' + str(prefix), 'USE_ASM=No', 'BUILDTYPE=Release']
    if machine: command.append('ARCH=' + machine)
    run(*command, '-j' + str(jobs), 'libraries', cwd=build, env=env)
    run(*command, 'install-shared', cwd=build, env=env)
    notice = prefix / 'share/licenses/openh264'; notice.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / 'LICENSE', notice / 'LICENSE')


LIBVTERM_FILES = ['include/vterm.h', 'include/vterm_keycodes.h', 'lib/libvterm.a',
                  'share/licenses/libvterm/LICENSE', 'lib/cmake/VTerm/VTermConfig.cmake',
                  'lib/cmake/VTerm/VTermConfigVersion.cmake',
                  'lib/cmake/VTerm/VTermTargets.cmake', 'lib/cmake/VTerm/VTermTargets-release.cmake']


def validate_libvterm_sdk(prefix, workspace, cmake, env, cross=False):
    workspace.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vterm-check-', dir=workspace) as directory:
        root = Path(directory)
        (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.24)
project(VTermProbe LANGUAGES C)
find_package(VTerm 0.3.3 EXACT REQUIRED CONFIG)
add_executable(probe probe.c)
target_link_libraries(probe PRIVATE VTerm::vterm)
''')
        (root / 'probe.c').write_text('''#include <vterm.h>
int main(void) {
    VTERM_CHECK_VERSION;
    VTerm *vt = vterm_new(2, 3);
    if (!vt) return 1;
    vterm_set_utf8(vt, 1);
    VTermScreen *screen = vterm_obtain_screen(vt);
    vterm_screen_reset(screen, 1);
    vterm_input_write(vt, "ok", 2);
    VTermScreenCell cell;
    int ok = vterm_screen_get_cell(screen, (VTermPos){0, 0}, &cell) && cell.chars[0] == 'o';
    vterm_free(vt);
    return ok ? 0 : 2;
}
''')
        run(cmake, '-S', root, '-B', root / 'build', '-G', 'Ninja',
            '-DCMAKE_PREFIX_PATH=' + str(prefix), env=env)
        run(cmake, '--build', root / 'build', env=env)
        # A cross-architecture probe links against the SDK but cannot execute on this host.
        if not cross: run(root / 'build/probe', env=env)


def install_libvterm(source, build, prefix, cmake, jobs, env, contract, cross=False):
    if not dependency_current(prefix, 'libvterm', contract, LIBVTERM_FILES):
        run(cmake, '-S', ROOT / 'cmake/libvterm', '-B', build, '-G', 'Ninja',
            '-DRM_LIBVTERM_SOURCE=' + str(source), '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib', env=env)
        run(cmake, '--build', build, '--parallel', str(jobs), env=env)
        run(cmake, '--install', build, env=env)
        record_dependency(prefix, 'libvterm', contract, LIBVTERM_FILES)
    validate_libvterm_sdk(prefix, build.parent, cmake, env, cross)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--platform', choices=['macos', 'linux'], required=True)
    p.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    p.add_argument('--arch', choices=['arm64', 'amd64'], help='macOS only: cross-build native SDK for another CPU')
    args = p.parse_args()
    if args.platform == 'macos':
        if platform.system() != 'Darwin':
            raise SystemExit('macOS bootstrap requires macOS, not a Linux container')
        for command in [('xcode-select', '-p'), ('xcrun', '--find', 'clang'), ('xcrun', '--show-sdk-path')]:
            run(*command)
        if not shutil.which('perl') or not shutil.which('make'):
            raise SystemExit('Apple Command Line Tools, perl and make required; no system install performed')
    if args.platform == 'linux' and platform.system() != 'Linux':
        raise SystemExit('Linux bootstrap requires Linux; on macOS use scripts/linux.sh image')
    if platform.machine() not in ('arm64', 'aarch64', 'x86_64', 'AMD64'):
        raise SystemExit('Only ARM64 and AMD64 SDK targets are supported')
    host_arch = 'arm64' if platform.machine() in ('arm64', 'aarch64') else 'amd64'
    arch = args.arch or host_arch
    cross = arch != host_arch
    if cross and args.platform != 'macos':
        raise SystemExit('Cross-architecture SDKs are supported only on macOS; use the matching Linux container')
    machine = 'x86_64' if arch == 'amd64' else 'arm64'
    dev = ROOT / '.dev' / args.platform
    cache = dev / 'downloads'
    cache.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    a = lock['artifacts']
    target = 'macos' if args.platform == 'macos' else 'linux-' + arch
    def artifact(key):
        return audit.fetch(a[key], cache)
    fetch_rust_notices(a, artifact)
    tools = dev / 'tools'
    cmake = install_build_tools(tools, target, lock['versions'], artifact)
    qt = dev / 'qt'
    qtprefix = qt
    install_qt_modules(qt, target, a, artifact)
    validate_shadertools(qt, ROOT / 'build' / args.platform / 'deps')
    env = dict(os.environ, CARGO_HOME=str(dev / 'cargo'), RUSTUP_HOME=str(dev / 'rustup'),
               CARGO_TARGET_DIR=str(ROOT / 'build' / args.platform / 'cargo'),
               XDG_CACHE_HOME=str(dev / 'cache'))
    env['PATH'] = os.pathsep.join([str(dev / 'cargo/bin'), str(cmake.parent), str(tools / 'bin'), env['PATH']])
    # Vendored tarballs carry no .git; keep their configure steps from seeing this checkout.
    ceiling = [str(ROOT)]
    if env.get('GIT_CEILING_DIRECTORIES'): ceiling.append(env['GIT_CEILING_DIRECTORIES'])
    env['GIT_CEILING_DIRECTORIES'] = os.pathsep.join(ceiling)
    if not (dev / 'cargo/bin/rustup').exists():
        installer = artifact('rustup-' + args.platform + '-' + host_arch)
        installer.chmod(0o755)
        run(installer, '-y', '--no-modify-path', '--profile', 'minimal', '--default-toolchain', '1.99.0', env=env)
    run(dev / 'cargo/bin/rustc', '--version', env=env)
    prefix = dev / 'prefix'
    source = dev / 'src'
    builds = ROOT / 'build' / args.platform / 'deps'
    if cross:
        # Host SDK paths stay untouched; the cross SDK lives in its own architecture directory.
        prefix = dev / arch / 'prefix'
        builds = ROOT / 'build' / args.platform / arch / 'deps'
        flags = '-arch ' + machine
        env.update(CMAKE_OSX_ARCHITECTURES=machine, CFLAGS=flags, CXXFLAGS=flags, LDFLAGS=flags)
        run(dev / 'cargo/bin/rustup', 'target', 'add', machine + '-apple-darwin', env=env)
    for key in ('openssl', 'zlib', 'openh264', 'freerdp', 'libvterm'):
        version = lock['versions'][key]
        src = source / (key + '-' + version)
        if not src.exists():
            unpack(artifact(key), source)
        if key == 'libvterm':
            contract = {'version': version, 'source': a[key]['sha256'], 'arch': arch,
                        'adapter': hashlib.sha256((ROOT / 'cmake/libvterm/CMakeLists.txt').read_bytes()).hexdigest()}
            install_libvterm(src, builds / key, prefix, cmake, args.jobs, env, contract, cross)
            continue
        if key == 'openssl':
            if (prefix / 'lib/libssl.a').exists():
                continue
            openssl_build = builds / 'openssl'
            openssl_build.mkdir(parents=True, exist_ok=True)
            configure = ['perl', src / 'Configure', 'no-tests', 'shared', '--prefix=' + str(prefix),
                         '--openssldir=' + str(prefix / 'ssl'), '--libdir=lib']
            if args.platform == 'macos':
                configure += ['darwin64-arm64-cc' if arch == 'arm64' else 'darwin64-x86_64-cc']
            # The Configure target selects the CPU; exported CFLAGS would replace its optimization flags.
            openssl_env = {k: v for k, v in env.items() if k not in ('CFLAGS', 'CXXFLAGS', 'LDFLAGS')}
            run(*configure, cwd=openssl_build, env=openssl_env)
            run('make', '-j' + str(args.jobs), cwd=openssl_build, env=openssl_env)
            run('make', 'install_sw', cwd=openssl_build, env=openssl_env)
            continue
        if key == 'zlib' and (prefix / 'include/zlib.h').exists():
            continue
        if key == 'openh264':
            contract = {'version': version, 'source': a[key]['sha256'], 'assembly': False, 'arch': arch}
            files = ['include/wels/' + name for name in ('codec_api.h', 'codec_app_def.h', 'codec_def.h', 'codec_ver.h')]
            files += [str(shared_library(prefix, key, args.platform).relative_to(prefix)), 'share/licenses/openh264/LICENSE']
            if not dependency_current(prefix, key, contract, files):
                install_openh264(src, builds / key, prefix, args.jobs, env, machine if cross else None)
                record_dependency(prefix, key, contract, files)
            continue
        if key == 'freerdp':
            patch_freerdp(src)
            contract = {'version': version, 'source': a[key]['sha256'], 'arch': arch,
                        'decoder': lock['versions']['openh264'], 'decoder_source': a['openh264']['sha256'],
                        'loader_patch': OPENH264_LOADER_PATCH, 'options': freerdp_options(prefix, args.platform)}
            if args.platform == 'macos':
                # Homebrew libraries are unpinned and host-CPU only; the SDK must not link them.
                contract['options'] += ['-DWITH_URIPARSER=OFF', '-DWITH_JSON_DISABLED=ON',
                                        '-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local']
            files = ['include/freerdp3/freerdp/' + name for name in ('freerdp.h', 'config.h', 'build-config.h')]
            files += [str(shared_library(prefix, name, args.platform).relative_to(prefix))
                      for name in ('freerdp3', 'freerdp-client3', 'winpr3', 'openh264')]
            files += ['include/wels/codec_ver.h', 'share/licenses/openh264/LICENSE']
            if dependency_current(prefix, key, contract, files):
                validate_freerdp_sdk(prefix, builds / key, contract['options'])
                continue
        options = ['-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
                   '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_PREFIX_PATH=' + str(prefix),
                   '-DCMAKE_INSTALL_RPATH=' + str(prefix / 'lib')]
        if key == 'freerdp':
            options += contract['options']
        run(cmake, '-S', src, '-B', builds / key, '-G', 'Ninja', *options, env=env)
        run(cmake, '--build', builds / key, '--parallel', str(args.jobs), env=env)
        run(cmake, '--install', builds / key, env=env)
        if key == 'freerdp':
            validate_freerdp_sdk(prefix, builds / key, contract['options'])
            record_dependency(prefix, key, contract, files)
    if cross:
        for name in ('ssl', 'crypto', 'openh264', 'freerdp3', 'freerdp-client3', 'winpr3'):
            library = shared_library(prefix, name, args.platform)
            if subprocess.check_output(['lipo', '-archs', str(library)], text=True).split() != [machine]:
                raise RuntimeError('Cross SDK library has the wrong architecture: ' + str(library))
    print('Ready:', dev, '\nNative SDK:', prefix, '\nQt:', qtprefix)


if __name__ == '__main__':
    main()
