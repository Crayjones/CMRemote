#!/usr/bin/env python3
"""Check a packaged macOS app runs from a copy with nothing from the build machine available."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True

SYSTEM = ('/System/', '/usr/lib/', '/Library/Apple/')


def macho_files(bundle):
    for path in bundle.rglob('*'):
        if path.is_file() and not path.is_symlink() and b'Mach-O' in subprocess.run(
                ['file', '-b', str(path)], capture_output=True).stdout:
            yield path


def audit_loads(bundle):
    frameworks = bundle / 'Contents/Frameworks'
    for path in macho_files(bundle):
        for arch in subprocess.check_output(['lipo', '-archs', str(path)], text=True).split():
            listing = subprocess.check_output(['otool', '-arch', arch, '-L', str(path)], text=True)
            for dependency in re.findall(r'^\t(\S+) \(', listing, re.M):
                if dependency.startswith(SYSTEM): continue
                name = re.sub(r'^@(rpath|executable_path/../Frameworks|loader_path(/../Frameworks)?)/', '', dependency)
                if name == dependency or not ((frameworks / name).exists() or (path.parent / name).exists()):
                    raise RuntimeError(f'{path.relative_to(bundle)} ({arch}) needs {dependency}, which the app does not ship')


def runnable(arch):
    return subprocess.run(['arch', '-' + arch, '/usr/bin/true'], capture_output=True).returncode == 0


def smoke(bundle, temp):
    executable = bundle / 'Contents/MacOS/RemoteManager'
    arch = subprocess.check_output(['lipo', '-archs', str(executable)], text=True).split()[0]
    if not runnable(arch):
        print(f'::warning::Launch check skipped: this host cannot run {arch} (install Rosetta 2)')
        return
    home, scratch, empty = temp / 'home', temp / 'tmp', temp / 'no-openssl-modules'
    for directory in (home, scratch, empty): directory.mkdir()
    # A fresh user: no SDK, no Homebrew, no OpenSSL modules from the build machine's baked-in path.
    env = dict(PATH='/usr/bin:/bin', HOME=str(home), TMPDIR=str(scratch), OPENSSL_MODULES=str(empty))
    # arch is a protected binary that drops DYLD_* from its own environment; -e hands it to the app.
    result = subprocess.run(['arch', '-' + arch, '-e', 'DYLD_PRINT_LIBRARIES=1', str(executable), '--smoke'], env=env,
                            capture_output=True, text=True, timeout=180)
    report = result.stdout + result.stderr
    if result.returncode: raise RuntimeError(f'Packaged app --smoke exited {result.returncode}:\n{report}')
    allowed = (str(bundle.resolve()) + '/', str(home.resolve()) + '/')
    loaded = re.findall(r'^dyld\[\d+\]: <[^>]*> (.+)$', report, re.M)
    if not loaded: raise RuntimeError('dyld did not report loaded images; cannot audit the launch')
    outside = sorted({p for p in loaded if not p.startswith(SYSTEM) and not os.path.realpath(p).startswith(allowed)})
    if outside: raise RuntimeError('Packaged app loaded files from outside the bundle:\n' + '\n'.join(outside))
    print(f'Launched {arch} copy: {len(loaded)} images, all from the bundle or macOS; RDP MD4/RC4 available')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    args = parser.parse_args()
    if sys.platform != 'darwin': parser.error('Run on macOS')
    if not (args.bundle / 'Contents/MacOS/RemoteManager').is_file(): parser.error('Not a packaged RemoteManager .app')
    with tempfile.TemporaryDirectory(prefix='cmremote-package-') as directory:
        temp = Path(directory).resolve()
        bundle = temp / 'CMRemote.app'
        # A relocated copy proves nothing resolves through the original build path.
        subprocess.run(['ditto', str(args.bundle), str(bundle)], check=True)
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)
        audit_loads(bundle)
        print('Every Mach-O dependency is shipped in the bundle or provided by macOS')
        smoke(bundle, temp)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
