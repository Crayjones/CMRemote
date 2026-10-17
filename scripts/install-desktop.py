#!/usr/bin/env python3
"""Opt in to a per-user Linux menu entry for this portable CMRemote directory."""
import os
from pathlib import Path
import sys

DESKTOP_ID = 'org.remotemanager.RemoteManager.desktop'


def exec_path(path):
    value = str(path)
    if any(char in value for char in '\n\r\x00'):
        raise ValueError('Unsupported control character in package path')
    value = value.replace('%', '%%')
    # Exec quoting is parsed after the desktop entry's string escapes.
    for char in ('\\', '"', '`', '$'):
        value = value.replace(char, '\\' + char)
    return '"' + value.replace('\\', '\\\\') + '"'


def install(bundle, data_home):
    launcher = bundle / 'RemoteManager'
    icon = bundle / 'share/icons/hicolor/256x256/apps/cmremote.png'
    template = bundle / 'share/applications' / DESKTOP_ID
    if not launcher.is_file() or not os.access(launcher, os.X_OK) or not icon.is_file():
        raise RuntimeError('Run the installer from a complete portable CMRemote package')
    text = template.read_text()
    lines = []
    for line in text.splitlines():
        if line.startswith('Exec='):
            line = 'Exec=' + exec_path(launcher)
        elif line.startswith('Icon='):
            line = 'Icon=' + str(icon).replace('\\', '\\\\')
        lines.append(line)
    target = data_home / 'applications' / DESKTOP_ID
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink():
        raise ValueError('Refusing a symlink desktop-entry destination')
    if target.exists() and 'X-cmRemote-Portable=true' not in target.read_text():
        raise ValueError('Refusing to replace an unrelated desktop entry: ' + str(target))
    target.write_text('\n'.join(lines) + '\nX-cmRemote-Portable=true\n')
    return target


def main():
    if not sys.platform.startswith('linux'):
        raise SystemExit('Desktop entry installation is Linux-only')
    bundle = Path(__file__).resolve().parent
    data_home = Path(os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share'))).expanduser()
    print('Installed:', install(bundle, data_home))
    print('If you move the package, run this script again to update its paths.')


if __name__ == '__main__':
    main()
