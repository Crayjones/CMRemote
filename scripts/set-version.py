#!/usr/bin/env python3
"""Set the CMRemote release version. CMakeLists.txt is the single source; the app,
installers and About dialog all read it. Used by the GitHub release workflow."""
import argparse
from pathlib import Path
import re
import sys
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
PATTERN = re.compile(r'(project\(RemoteManager VERSION )(\d+\.\d+\.\d+)( )')


def current(root=ROOT):
    match = PATTERN.search((root / 'CMakeLists.txt').read_text())
    if not match: raise SystemExit('CMakeLists.txt has no project version')
    return match.group(2)


def parse(version):
    version = version.strip().removeprefix('v')
    if not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', version):
        raise SystemExit('Version must look like 1.2.3: ' + version)
    return version


def set_version(version, root=ROOT):
    version = parse(version)
    path = root / 'CMakeLists.txt'
    text = path.read_text()
    if tuple(map(int, version.split('.'))) < tuple(map(int, current(root).split('.'))):
        raise SystemExit('Version ' + version + ' is older than ' + current(root))
    path.write_text(PATTERN.sub(lambda m: m.group(1) + version + m.group(3), text, count=1))
    return version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('version', nargs='?', help='New version, e.g. 0.2.0 (omit to print the current one)')
    args = parser.parse_args()
    print(set_version(args.version) if args.version else current())


if __name__ == '__main__':
    main()
