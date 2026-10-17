#!/usr/bin/env python3
"""Remove only known project-generated directories. Dry run unless --yes."""
import argparse
from pathlib import Path
import platform
import shutil

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ('qt', 'prefix', 'tools', 'src', 'python', 'cargo', 'rustup', 'downloads', 'cache', 'vscode-server')


def targets(root, host, scope, include_data=False):
    hosts = ('macos', 'linux') if host == 'both' else (host,)
    result = []
    for h in hosts:
        if scope in ('build', 'all'): result.append(root / 'build' / h)
        if scope in ('dist', 'all'): result.append(root / 'dist' / h)
        if scope in ('tools', 'all'): result.extend(root / '.dev' / h / name for name in TOOLS)
        if scope == 'data' or include_data: result.append(root / '.dev' / h / 'data')
    for path in result:
        if root.resolve() not in path.resolve().parents:
            raise ValueError('cleanup target escapes project: ' + str(path))
        for parent in [path] + list(path.parents):
            if parent == root: break
            if parent.is_symlink(): raise ValueError('cleanup target has a symlink: ' + str(parent))
    return result


def remove(paths):
    for path in paths:
        if path.is_dir(): shutil.rmtree(path)
        elif path.exists(): path.unlink()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--platform', choices=['macos', 'linux', 'both'], default='macos' if platform.system() == 'Darwin' else 'linux')
    p.add_argument('--scope', choices=['build', 'dist', 'tools', 'data', 'all'], required=True)
    p.add_argument('--include-data', action='store_true', help='also remove development profiles; never normal app-data')
    p.add_argument('--yes', action='store_true')
    args = p.parse_args()
    paths = targets(ROOT, args.platform, args.scope, args.include_data)
    for path in paths: print(path, '(exists)' if path.exists() else '(absent)')
    if args.yes:
        remove(paths)
    else:
        print('Dry run. Add --yes to remove these directories. Docker resources require scripts/linux.sh clean.')


if __name__ == '__main__':
    main()
