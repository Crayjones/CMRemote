#!/usr/bin/env python3
"""Stage a portable development artifact. Not signed/notarized for distribution."""
import argparse
import re
import json
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True
from env import ROOT, environment


def stage_decoder_notice(native, destination, host):
    # No OpenH264 is shipped: the app downloads Cisco's binary itself (app/OpenH264.cpp).
    # Cisco's binary license and its patent notice travel in vendor/licenses.
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    relative = 'Contents/Resources/third-party' if host == 'macos' else 'share/third-party'
    notices = destination / relative; notices.mkdir(parents=True, exist_ok=True)
    (notices.parent / 'dependency-versions.json').write_text(json.dumps(lock['versions'], indent=2) + '\n')
    (notices / 'OpenH264-NOTICE.txt').write_text(
        'OpenH264 Video Codec provided by Cisco Systems, Inc.\n\n'
        'CMRemote does not include OpenH264. When H.264 video is turned on (Options > Remote Desktop),\n'
        'CMRemote downloads Cisco\'s prebuilt OpenH264 ' + lock['versions']['openh264'] + ' from\n'
        'http://ciscobinary.openh264.org/ onto this device, checks it against a pinned SHA-256 and loads it.\n'
        'Turning the option off stops its use. Its license is OpenH264-Cisco-BINARY_LICENSE.txt.\n')


def reject_bundled_openh264(destination):
    shipped = [p for p in destination.rglob('*') if p.name.startswith(('libopenh264', 'openh264')) and p.suffix != '.txt']
    if shipped: raise RuntimeError('Artifact must not contain OpenH264: ' + ', '.join(str(p) for p in shipped))


def stage_libvterm_notice(native, destination, host):
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    manifest = json.loads((native / '.rm-libvterm.json').read_text())
    path = 'share/licenses/libvterm/LICENSE'
    license = (native / path).read_bytes()
    if manifest.get('contract', {}).get('version') != lock['versions']['libvterm'] or \
            manifest.get('contract', {}).get('source') != lock['artifacts']['libvterm']['sha256'] or \
            manifest.get('files', {}).get(path) != hashlib.sha256(license).hexdigest():
        raise RuntimeError('libvterm notice does not match the pinned installed dependency')
    relative = 'Contents/Resources/third-party' if host == 'macos' else 'share/third-party'
    notices = destination / relative; notices.mkdir(parents=True, exist_ok=True)
    (notices / 'libvterm-LICENSE.txt').write_bytes(license)
    (notices / 'libvterm-NOTICE.txt').write_text(
        'libvterm ' + lock['versions']['libvterm'] + '\nSource: ' + lock['artifacts']['libvterm']['url'] +
        '\nSHA-256: ' + lock['artifacts']['libvterm']['sha256'] + '\nLinked statically; no libvterm SDK runtime required.\n')


def stage_ssh_notices(native, destination, host, env):
    from rust_notices import stage
    stage_libvterm_notice(native, destination, host)
    relative = 'Contents/Resources/third-party' if host == 'macos' else 'share/third-party'
    stage(destination / relative / 'rust', env)


def stage_ui_notices(destination, host, env=None):
    from ui_assets import stage_assets
    relative = 'Contents/Resources/third-party' if host == 'macos' else 'share/third-party'
    stage_assets(destination / relative / 'mremoteng-assets', project_root=ROOT, include_images=False)
    # CMRemote's own GPL-2.0-or-later license travels with every package.
    shutil.copy2(ROOT / 'LICENSE', destination / relative / '..' / 'LICENSE.txt')
    # Qt, FreeRDP, OpenSSL and zlib ship as binaries; their license texts must travel too.
    for notice in (ROOT / 'vendor/licenses').iterdir():
        shutil.copy2(notice, destination / relative / notice.name)
    qt = (env or os.environ).get('RM_QT_PREFIX')
    sbom = Path(qt or '.') / 'sbom'
    if qt and sbom.is_dir():
        (destination / relative / 'qt-sbom').mkdir(parents=True, exist_ok=True)
        for spdx in sbom.glob('*.spdx.json'):
            shutil.copy2(spdx, destination / relative / 'qt-sbom' / spdx.name)


def stage_branding(destination, host, source, build):
    from branding import SIZES, verify
    assets = ROOT / 'app/assets/branding'
    manifest = verify(assets)
    relative = 'Contents/Resources' if host == 'macos' else 'share'
    resources = destination / relative
    if host == 'macos':
        info = plistlib.loads((destination / 'Contents/Info.plist').read_bytes())
        if info.get('CFBundleIconFile') != 'cmremote.icns' or info.get('CFBundleName') != 'CMRemote' or \
                info.get('CFBundleDisplayName') != 'CMRemote' or info.get('CFBundleExecutable') != 'RemoteManager':
            raise RuntimeError('Build the current CMRemote bundle before packaging')
        if (resources / 'cmremote.icns').read_bytes() != (assets / 'cmremote.icns').read_bytes():
            raise RuntimeError('Bundle icon differs from the current branding assets')
        executable = source / 'Contents/MacOS/RemoteManager'
    elif host == 'windows':
        executable = source
        resources.mkdir(parents=True, exist_ok=True)
        shutil.copy2(assets / 'cmremote.ico', resources / 'cmremote.ico')
    else:
        for size in SIZES:
            directory = resources / ('icons/hicolor/' + str(size) + 'x' + str(size) + '/apps')
            directory.mkdir(parents=True, exist_ok=True)
            shutil.copy2(assets / ('cmremote-' + str(size) + '.png'), directory / 'cmremote.png')
        applications = resources / 'applications'; applications.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / 'app/assets/cmremote.desktop', applications / 'org.remotemanager.RemoteManager.desktop')
        shutil.copy2(ROOT / 'scripts/install-desktop.py', destination / 'install-desktop.py')
        executable = source
    branding = resources / 'branding'; branding.mkdir(parents=True, exist_ok=True)
    shutil.copy2(assets / 'manifest.json', branding / 'manifest.json')
    shutil.copy2(assets / 'favicon.ico', branding / 'favicon.ico')
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    info = dict(schema=1, product='CMRemote', platform=host, source_build=str(build.relative_to(ROOT)),
                source_executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                branding_source_sha256=manifest['source_sha256'],
                branding_manifest_sha256=hashlib.sha256((assets / 'manifest.json').read_bytes()).hexdigest(),
                dependency_lock_sha256=hashlib.sha256((ROOT / 'dependencies.lock.json').read_bytes()).hexdigest(),
                dependency_versions=lock['versions'])
    (resources / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')


# DLLs every supported Windows 10/11 x64 install provides; anything else must ship in the package.
WINDOWS_SYSTEM_DLLS = {
    'advapi32', 'bcrypt', 'bcryptprimitives', 'combase', 'comctl32', 'comdlg32', 'credui', 'crypt32',
    'd3d11', 'd3d12', 'd3d9', 'dcomp', 'dnsapi', 'dwmapi', 'dwrite', 'dxgi', 'gdi32', 'imm32', 'iphlpapi',
    'kernel32', 'mpr', 'msvcrt', 'ncrypt', 'netapi32', 'ntdll', 'ole32', 'oleaut32', 'opengl32', 'rpcrt4',
    'secur32', 'setupapi', 'shcore', 'shell32', 'shlwapi', 'ucrtbase', 'user32', 'userenv', 'uxtheme',
    'version', 'winmm', 'winspool.drv', 'ws2_32', 'wtsapi32', 'authz', 'dbghelp', 'wininet', 'winhttp',
    'cfgmgr32', 'd2d1', 'hid', 'mscms', 'mswsock', 'normaliz', 'powrprof', 'propsys', 'psapi', 'urlmon', 'wldap32',
}
WINDOWS_PLUGIN_DIRS = ('platforms', 'styles', 'imageformats', 'tls', 'networkinformation', 'iconengines')


def windows_system_dll(name):
    name = name.lower()
    return name.startswith(('api-ms-win-', 'ext-ms-')) or name.removesuffix('.dll') in WINDOWS_SYSTEM_DLLS


def stage_windows(source, destination, env):
    qt, native, mingw = Path(env['RM_QT_PREFIX']), Path(env['RM_NATIVE_PREFIX']), Path(env['RM_LLVM_MINGW'])
    search = [qt / 'bin', native / 'bin', mingw / 'x86_64-w64-mingw32/bin']
    destination.mkdir(parents=True)
    shutil.copy2(source, destination / 'RemoteManager.exe')
    for directory in WINDOWS_PLUGIN_DIRS:
        plugins = qt / 'plugins' / directory
        if not plugins.is_dir(): continue
        for plugin in sorted(plugins.glob('*.dll')):
            # Qt ships debug builds beside release ones as <name>d.dll.
            if plugin.stem.endswith('d') and (plugins / (plugin.stem[:-1] + '.dll')).exists(): continue
            target = destination / directory / plugin.name
            target.parent.mkdir(exist_ok=True); shutil.copy2(plugin, target)
    pending = [destination / 'RemoteManager.exe'] + list(destination.glob('*/*.dll'))
    staged, unknown = set(), set()
    while pending:
        path = pending.pop()
        imports = subprocess.check_output(['llvm-objdump', '-p', str(path)], text=True, env=env)
        for name in re.findall(r'DLL Name: (\S+)', imports):
            if name.lower() in staged or windows_system_dll(name): continue
            found = next((d / name for d in search if (d / name).is_file()), None)
            if not found:
                unknown.add(name); continue
            staged.add(name.lower())
            shutil.copy2(found, destination / name)
            pending.append(destination / name)
    if unknown: raise RuntimeError('Unshipped non-system DLL dependencies: ' + ', '.join(sorted(unknown)))
    for path in [destination / 'RemoteManager.exe', *destination.rglob('*.dll')]:
        subprocess.run(['llvm-strip', '--strip-unneeded', str(path)], env=env, check=True)
    reject_bundled_openh264(destination)
    if not (destination / 'platforms/qwindows.dll').is_file(): raise RuntimeError('Artifact is missing the Qt Windows platform plugin')


def macho_runtime_paths(output):
    return re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset \d+\)', output)


def audit_macho_loads(output):
    for line in output.splitlines():
        if not line.startswith('\t'): continue
        dependency = line.strip().split(' (', 1)[0]
        if not dependency.startswith(('@rpath/', '@loader_path/', '@executable_path/', '/usr/lib/', '/System/Library/')):
            raise RuntimeError('Artifact has an uncontained Mach-O dependency')


def audit_macho_runtime(runtime, bundle, loader, executable):
    if runtime.startswith(('/usr/lib/', '/System/Library/')): return
    for token, anchor in (('@loader_path/', loader.parent), ('@executable_path/', executable.parent)):
        if runtime.startswith(token) and (anchor / runtime[len(token):]).resolve().is_relative_to(bundle.resolve()): return
    raise RuntimeError('Artifact has an uncontained runtime search path')


def within_prefix(path, prefixes):
    # Case-insensitive volumes accept any spelling of a prefix; ask the filesystem, not the string.
    path = Path(path)
    if any(path.is_relative_to(prefix) for prefix in prefixes): return True
    existing = [prefix for prefix in prefixes if prefix.exists()]
    return any(ancestor.exists() and os.path.samefile(ancestor, prefix)
               for ancestor in (path, *path.parents) for prefix in existing)


def relocate_macho_bundle(bundle, env):
    executable = bundle / 'Contents/MacOS/RemoteManager'
    prefixes = (ROOT / '.dev', ROOT / 'build', Path(env['RM_QT_PREFIX']), Path(env['RM_NATIVE_PREFIX']))
    for path in bundle.rglob('*'):
        if path.is_symlink():
            if not path.resolve().is_relative_to(bundle.resolve()): raise RuntimeError('Artifact symlink escapes bundle')
            continue
        if not path.is_file(): continue
        loads = subprocess.run(['otool', '-L', str(path)], capture_output=True, text=True)
        if loads.returncode != 0: continue
        audit_macho_loads(loads.stdout)
        commands = subprocess.check_output(['otool', '-l', str(path)], text=True)
        for runtime in set(macho_runtime_paths(commands)):
            if runtime.startswith('/') and within_prefix(runtime, prefixes):
                subprocess.run(['install_name_tool', '-delete_rpath', runtime, str(path)], check=True)
            else:
                audit_macho_runtime(runtime, bundle, path, executable)
        for runtime in macho_runtime_paths(subprocess.check_output(['otool', '-l', str(path)], text=True)):
            audit_macho_runtime(runtime, bundle, path, executable)
    reject_bundled_openh264(bundle)
    subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(bundle)], check=True, capture_output=True)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(bundle)], check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact-name', default='RemoteManager', help='Use a separate contained artifact without replacing an existing one')
    args = parser.parse_args()
    if not re.fullmatch('[A-Za-z0-9][A-Za-z0-9._-]{0,63}', args.artifact_name): parser.error('Invalid artifact name')
    env = environment()
    host = env['RM_PLATFORM']
    build = Path(env.get('RM_APP_BUILD_DIR', str(Path(env['RM_BUILD_DIR']) / 'app')))
    dist = ROOT / 'dist' / host
    if host == 'linux': dist /= Path(env['RM_BUILD_DIR']).name
    dist.mkdir(parents=True, exist_ok=True)
    destination = dist / (args.artifact_name + ('.app' if host == 'macos' else ''))
    archive = dist / (args.artifact_name + {'macos': '-macos.zip', 'windows': '-windows.zip'}.get(host, '-linux-' + dist.name + '.tar.gz'))
    if archive.exists(): raise SystemExit('Archive already exists: ' + str(archive))
    if destination.exists():
        raise SystemExit('Artifact already exists: ' + str(destination) + '. Move it or run cleanup --scope dist --yes.')
    qt = Path(env['RM_QT_PREFIX'])
    native = Path(env['RM_NATIVE_PREFIX'])
    if host == 'macos':
        source = build / 'RemoteManager.app'
        if not source.exists(): raise SystemExit('Build the application first')
        shutil.copytree(source, destination, symlinks=True)
        stage_decoder_notice(native, destination, host)
        stage_ssh_notices(native, destination, host, env)
        stage_ui_notices(destination, host, env)
        stage_branding(destination, host, source, build)
        reject_bundled_openh264(destination)
        subprocess.run([str(qt / 'bin/macdeployqt'), str(destination), '-always-overwrite',
                        '-libpath=' + str(native / 'lib'), '-codesign=-'], env=env, check=True)
        relocate_macho_bundle(destination, env)
        subprocess.run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', str(destination), str(archive)], check=True)
    elif host == 'windows':
        source = build / 'RemoteManager.exe'
        if not source.exists(): raise SystemExit('Build the application first')
        stage_windows(source, destination, env)
        stage_decoder_notice(native, destination, host)
        stage_ssh_notices(native, destination, host, env)
        stage_ui_notices(destination, host, env)
        stage_branding(destination, host, source, build)
        reject_bundled_openh264(destination)
        shutil.make_archive(str(archive).removesuffix('.zip'), 'zip', dist, destination.name)
    else:
        source = build / 'RemoteManager'
        if not source.exists(): raise SystemExit('Build the application first')
        provider = native / 'lib/ossl-modules/legacy.so'
        required = [qt / 'plugins/xcbglintegrations' / name for name in
                    ('libqxcb-glx-integration.so', 'libqxcb-egl-integration.so')]
        for path in [*required, provider]:
            if not path.is_file(): raise SystemExit('Missing required Linux runtime: ' + str(path))
        (destination / 'bin').mkdir(parents=True)
        (destination / 'lib/ossl-modules').mkdir(parents=True)
        shutil.copy2(source, destination / 'bin/RemoteManager')
        shutil.copy2(provider, destination / 'lib/ossl-modules/legacy.so')
        # tls carries Qt's OpenSSL backend for Check for Updates (HTTPS to GitHub).
        for directory in ('platforms', 'imageformats', 'xcbglintegrations', 'tls', 'wayland-decoration-client', 'wayland-graphics-integration-client', 'wayland-shell-integration'):
            plugins = qt / 'plugins' / directory
            if plugins.exists():
                for plugin in plugins.glob('*.so'):
                    linked = subprocess.run(['ldd', str(plugin)], capture_output=True, text=True, env=env, check=True).stdout
                    if 'libQt6Svg.so.6 => not found' in linked:
                        print('Omitting optional SVG-dependent plugin:', plugin.name)
                        continue
                    if 'not found' in linked: raise SystemExit('Missing plugin runtime dependency: ' + linked)
                    target = destination / 'plugins' / directory / plugin.name
                    target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(plugin, target)
        pending = [destination / 'bin/RemoteManager', destination / 'lib/ossl-modules/legacy.so'] + list((destination / 'plugins').rglob('*.so'))
        seen = set()
        while pending:
            path = pending.pop()
            result = subprocess.run(['ldd', str(path)], capture_output=True, text=True, env=env, check=True)
            if 'not found' in result.stdout: raise SystemExit('Missing runtime dependency: ' + result.stdout)
            for line in result.stdout.splitlines():
                fields = line.strip().split()
                if '=>' not in fields or len(fields) < 3: continue
                dependency = Path(fields[2])
                if not dependency.is_absolute() or dependency in seen: continue
                seen.add(dependency)
                if any(str(dependency).startswith(str(prefix) + os.sep) for prefix in (qt, native)):
                    target = destination / 'lib' / dependency.name
                    shutil.copy2(dependency.resolve(), target)
                    pending.append(target)
        for path in [destination / 'bin/RemoteManager'] + [p for p in (destination / 'lib').rglob('*') if p.is_file()] + list((destination / 'plugins').rglob('*.so')):
            relative = os.path.relpath(destination / 'lib', path.parent)
            subprocess.run(['patchelf', '--set-rpath', '$ORIGIN/' + relative, str(path)], env=env, check=True)
        (destination / 'bin/qt.conf').write_text('[Paths]\nPrefix=..\nLibraries=lib\nPlugins=plugins\n')
        launcher = destination / 'RemoteManager'
        launcher.write_text('#!/bin/sh\nset -eu\nDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexport LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\nexport QT_PLUGIN_PATH="$DIR/plugins"\nexport OPENSSL_MODULES="$DIR/lib/ossl-modules"\nexec "$DIR/bin/RemoteManager" "$@"\n')
        launcher.chmod(0o755)
        (destination / 'RUNTIME.txt').write_text('Requires Ubuntu 24.04-compatible glibc and the system graphics/X11/Wayland libraries listed in Dockerfile. Not an AppImage.\nXCB GLX/EGL plugins and the matching OpenSSL legacy provider are bundled. Use the top-level RemoteManager launcher.\nA physical GPU is not required when Mesa software OpenGL (llvmpipe) is available; install libgl1-mesa-dri and libglx-mesa0 on Debian/Ubuntu.\nWayland interaction is unverified.\n')
        stage_decoder_notice(native, destination, host)
        stage_ssh_notices(native, destination, host, env)
        stage_ui_notices(destination, host, env)
        stage_branding(destination, host, source, build)
        reject_bundled_openh264(destination)
        (destination / 'DESKTOP.txt').write_text('Run ./RemoteManager from any directory.\nOptional Linux menu entry: python3 install-desktop.py\nThe installer writes only your user desktop entry; rerun it after moving this package.\nThe share/applications desktop entry is a system-install template, not a relocatable shortcut.\n')
        shutil.make_archive(str(archive).removesuffix('.tar.gz'), 'gztar', dist, destination.name)
    print('Packaged:', destination)


if __name__ == '__main__':
    main()
