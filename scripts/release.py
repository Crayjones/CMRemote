#!/usr/bin/env python3
"""Wrap packaged artifacts into named installers under dist/<platform>/release/. Unsigned."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
PRODUCT = 'CMRemote'
APP_ID = 'org.remotemanager.RemoteManager'
FLATHUB = 'https://dl.flathub.org/repo/flathub.flatpakrepo'
# Flatpak/freedesktop icon sizes; larger branding PNGs are not valid hicolor app icons.
FLATPAK_ICON_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)


def version():
    match = re.search(r'project\(RemoteManager VERSION (\d+\.\d+\.\d+)', (ROOT / 'CMakeLists.txt').read_text())
    if not match: raise SystemExit('CMakeLists.txt has no project version')
    return match.group(1)


def release_name(platform, arch, suffix):
    return PRODUCT + '-' + version() + '-' + platform + '-' + arch + suffix


def run(*command, **kwargs):
    subprocess.run([str(x) for x in command], cwd=ROOT, check=True, **kwargs)


def macos_dmg(bundle, output):
    """Drag-to-Applications disk image around one packaged, ad-hoc-signed bundle."""
    run('codesign', '--verify', '--deep', '--strict', bundle)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists(): output.unlink()
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        stage = Path(temp) / 'stage'
        stage.mkdir()
        run('ditto', bundle, stage / (PRODUCT + '.app'))
        (stage / 'Applications').symlink_to('/Applications')
        run('hdiutil', 'create', '-quiet', '-volname', PRODUCT + ' ' + version(), '-srcfolder', stage,
            '-fs', 'HFS+', '-format', 'UDZO', '-ov', output)
    run('hdiutil', 'verify', '-quiet', output)
    return output


def lock():
    return json.loads((ROOT / 'dependencies.lock.json').read_text())


def pinned(key):
    """Download (once) and checksum-verify a locked release tool."""
    artifact = lock()['artifacts'][key]
    cache = ROOT / '.dev/linux/downloads'
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / artifact['file']
    if Path(artifact['file']).name != artifact['file'] or path.is_symlink():
        raise ValueError('Unsafe release tool path')
    if not path.exists():
        run('curl', '-fsSL', '--retry', '3', '-o', str(path) + '.part', artifact['url'])
        os.replace(str(path) + '.part', path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != artifact['sha256']:
        path.unlink()
        raise RuntimeError('Checksum mismatch for ' + key)
    path.chmod(0o755)
    return path


def desktop_entry(icon):
    lines = (ROOT / 'app/assets/cmremote.desktop').read_text().splitlines()
    return '\n'.join('Icon=' + icon if line.startswith('Icon=') else line for line in lines) + '\n'


def linux_appimage(package, work, output):
    appdir = work / (PRODUCT + '.AppDir')
    shutil.rmtree(appdir, ignore_errors=True)
    shutil.copytree(package, appdir, symlinks=True)
    apprun = appdir / 'AppRun'
    # The package launcher already resolves its own directory and sets library/plugin paths.
    apprun.write_text('#!/bin/sh\nset -eu\nHERE=${APPDIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}\n'
                      'exec "$HERE/RemoteManager" "$@"\n')
    apprun.chmod(0o755)
    (appdir / 'cmremote.desktop').write_text(desktop_entry('cmremote') + 'X-AppImage-Version=' + version() + '\n')
    shutil.copy2(ROOT / 'app/assets/branding/cmremote-256.png', appdir / 'cmremote.png')
    (appdir / '.DirIcon').symlink_to('cmremote.png')
    if output.exists(): output.unlink()
    run(pinned('appimagetool-linux-amd64'), '--no-appstream', '--runtime-file', pinned('appimage-runtime-linux-amd64'),
        appdir, output, env=dict(os.environ, ARCH='x86_64', APPIMAGE_EXTRACT_AND_RUN='1'))
    return output


def linux_flatpak(package, work, output):
    runtime = lock()['versions']['flatpak_kde_runtime']
    root = work / 'flatpak'
    shutil.rmtree(root, ignore_errors=True)
    build = root / 'build'
    files = build / 'files'
    shutil.copytree(package, files / 'lib/cmremote', symlinks=True)
    launcher = files / 'bin/RemoteManager'
    launcher.parent.mkdir(parents=True)
    launcher.write_text('#!/bin/sh\nexec /app/lib/cmremote/RemoteManager "$@"\n')
    launcher.chmod(0o755)
    share = files / 'share'
    (share / 'applications').mkdir(parents=True)
    (share / 'applications' / (APP_ID + '.desktop')).write_text(desktop_entry(APP_ID))
    for size in FLATPAK_ICON_SIZES:
        icons = share / 'icons/hicolor' / (str(size) + 'x' + str(size)) / 'apps'
        icons.mkdir(parents=True)
        shutil.copy2(ROOT / 'app/assets/branding' / ('cmremote-' + str(size) + '.png'), icons / (APP_ID + '.png'))
    (share / 'metainfo').mkdir()
    (share / 'metainfo' / (APP_ID + '.metainfo.xml')).write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<component type="desktop-application">\n'
        '  <id>' + APP_ID + '</id>\n  <name>' + PRODUCT + '</name>\n'
        '  <summary>Manage RDP, SSH and VNC connections</summary>\n'
        '  <metadata_license>CC0-1.0</metadata_license>\n'
        '  <launchable type="desktop-id">' + APP_ID + '.desktop</launchable>\n'
        '  <releases><release version="' + version() + '"/></releases>\n</component>\n')
    # Saved connection files and SSH keys live in the user's home directory.
    (build / 'metadata').write_text(
        '[Application]\nname=' + APP_ID + '\nruntime=org.kde.Platform/x86_64/' + runtime +
        '\nsdk=org.kde.Sdk/x86_64/' + runtime + '\ncommand=RemoteManager\n\n'
        '[Context]\nshared=network;ipc;\nsockets=x11;wayland;fallback-x11;\ndevices=dri;\nfilesystems=home;\n')
    run('flatpak', 'build-finish', build)
    repo = root / 'repo'
    run('flatpak', 'build-export', '--arch=x86_64', repo, build, 'stable')
    if output.exists(): output.unlink()
    run('flatpak', 'build-bundle', '--arch=x86_64', '--runtime-repo=' + FLATHUB, repo, output, APP_ID, 'stable')
    return output


NSIS_SCRIPT = r'''Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
!include "MUI2.nsh"
!include "x64.nsh"
!define PRODUCT "@PRODUCT@"
!define VERSION "@VERSION@"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"
Name "${PRODUCT} ${VERSION}"
OutFile "@OUTPUT@"
InstallDir "$PROGRAMFILES64\${PRODUCT}"
InstallDirRegKey HKLM "Software\${PRODUCT}" "InstallDir"
RequestExecutionLevel admin
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "FileDescription" "${PRODUCT} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
!define MUI_ICON "@ICON@"
!define MUI_UNICON "@ICON@"
!define MUI_FINISHPAGE_RUN "$INSTDIR\RemoteManager.exe"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${PRODUCT} requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd
Function un.onInit
  SetRegView 64
FunctionEnd
Function .onInstFailed
  ; Roll back: drop the partial copy and restore the version that was installed.
  IfFileExists "$INSTDIR.previous\RemoteManager.exe" 0 done
    RMDir /r "$INSTDIR"
    Rename "$INSTDIR.previous" "$INSTDIR"
  done:
FunctionEnd
Section "Install"
  SetShellVarContext all
  ; An interrupted earlier setup left the installed version aside: restore it first.
  IfFileExists "$INSTDIR.previous\RemoteManager.exe" 0 staged
    RMDir /r "$INSTDIR"
    Rename "$INSTDIR.previous" "$INSTDIR"
  staged:
  ; Move an installed version aside rather than deleting it: files dropped by an older
  ; version cannot be loaded, and a failed copy rolls back in .onInstFailed. A folder
  ; without CMRemote in it is never moved or removed.
  IfFileExists "$INSTDIR\RemoteManager.exe" 0 copy
    ClearErrors
    Rename "$INSTDIR" "$INSTDIR.previous"
    IfErrors 0 copy
      MessageBox MB_ICONSTOP "Close ${PRODUCT} and run Setup again. The installed version was not changed." /SD IDOK
      SetErrorLevel 2
      Quit
  copy:
  SetOutPath "$INSTDIR"
  ClearErrors
  File /r "@PACKAGE@\*"
  IfErrors 0 +2
    Abort "Copying ${PRODUCT} failed; the previous version is restored."
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\${PRODUCT}"
  CreateShortcut "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk" "$INSTDIR\RemoteManager.exe" "" "$INSTDIR\RemoteManager.exe" 0
  WriteRegStr HKLM "Software\${PRODUCT}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${PRODUCT}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\RemoteManager.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  RMDir /r "$INSTDIR.previous"
SectionEnd
Section "Uninstall"
  SetShellVarContext all
  Delete "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT}"
  ; Only the installed program; saved connections and settings live in the user's profile.
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "Software\${PRODUCT}"
SectionEnd
'''


def windows_setup(package, work, output):
    for required in ('RemoteManager.exe', 'platforms/qwindows.dll'):
        if not (package / required).is_file(): raise SystemExit('Not a packaged Windows tree: ' + str(package))
    script = work / 'cmremote.nsi'
    work.mkdir(parents=True, exist_ok=True)
    values = {'@PRODUCT@': PRODUCT, '@VERSION@': version(), '@OUTPUT@': str(output),
              '@ICON@': str(ROOT / 'app/assets/branding/cmremote.ico'), '@PACKAGE@': str(package)}
    text = NSIS_SCRIPT
    for key, value in values.items():
        if '"' in value: raise ValueError('Quote in installer path: ' + value)
        text = text.replace(key, value)
    script.write_text(text)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists(): output.unlink()
    run('makensis', '-V2', '-NOCD', script)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='target', required=True)
    mac = sub.add_parser('macos', help='DMG from a packaged .app')
    mac.add_argument('--arch', choices=['arm64', 'x64'], required=True)
    mac.add_argument('--bundle', type=Path, required=True, help='Packaged .app from scripts/package.py')
    windows = sub.add_parser('windows', help='NSIS setup.exe from a packaged tree (run via docker.py windows)')
    windows.add_argument('--package', type=Path, required=True, help='Packaged directory from scripts/package.py')
    linux = sub.add_parser('linux', help='AppImage and Flatpak from a packaged x86_64 tree (run via docker.py release)')
    linux.add_argument('--package', type=Path, required=True, help='Packaged directory from scripts/package.py')
    linux.add_argument('--format', choices=['appimage', 'flatpak'], action='append',
                       help='Formats to build (default: both)')
    args = parser.parse_args()
    if args.target == 'macos':
        output = ROOT / 'dist/macos/release' / release_name('macos', args.arch, '.dmg')
        print('Release:', macos_dmg(args.bundle.resolve(), output))
    elif args.target == 'windows':
        output = ROOT / 'dist/windows/release' / release_name('windows', 'x64', '-setup.exe')
        print('Release:', windows_setup(args.package.resolve(), ROOT / 'build/windows/release', output))
    elif args.target == 'linux':
        import platform
        if platform.system() != 'Linux' or platform.machine() != 'x86_64':
            raise SystemExit('Linux installers are built in the amd64 container: scripts/docker.py release --arch amd64')
        package = args.package.resolve()
        if not (package / 'RemoteManager').is_file(): raise SystemExit('Not a packaged Linux tree: ' + str(package))
        work = ROOT / 'build/linux/amd64/release'
        work.mkdir(parents=True, exist_ok=True)
        dist = ROOT / 'dist/linux/release'
        dist.mkdir(parents=True, exist_ok=True)
        for kind in args.format or ['appimage', 'flatpak']:
            build = linux_appimage if kind == 'appimage' else linux_flatpak
            print('Release:', build(package, work, dist / release_name('linux', 'x86_64', '.' + kind)))


if __name__ == '__main__':
    main()
