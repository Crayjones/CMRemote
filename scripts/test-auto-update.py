#!/usr/bin/env python3
"""Run the update helpers against disposable fixture installations; never a real CMRemote.

macOS: real disk images from scripts/release.py around synthetic CMRemote.app bundles,
parent-exit waiting, validation, replacement, rollback, result records and relaunch.
AppImage: the Linux helper on this host's /bin/sh, or with --linux-image inside an
owned Linux container (dash, GNU coreutils, a relaunchable ELF fixture).
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
IDENTIFIER = 'org.remotemanager.RemoteManager'
OLD, NEW = '1.0.0', '2.0.0'


def helper(kind):
    """The script the app writes into a session: common prologue + platform part."""
    return '#!/bin/sh\n' + (ROOT / 'app/update/common.sh').read_text() + (ROOT / 'app/update' / ('install-' + kind + '.sh')).read_text()


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def wait_for(condition, seconds, what):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition(): return
        time.sleep(0.05)
    raise AssertionError('Timed out waiting for ' + what)


def check(condition, message):
    if not condition: raise AssertionError(message)


def tree(path):
    """Every entry with its bytes or link, to prove unrelated files are untouched."""
    result = {}
    for entry in sorted(Path(path).rglob('*')):
        key = str(entry.relative_to(path))
        result[key] = ('link', os.readlink(entry)) if entry.is_symlink() else ('dir',) if entry.is_dir() else ('file', entry.read_bytes())
    return result


class Attempt:
    """One helper run: private session, a stand-in CMRemote process, the helper itself."""

    def __init__(self, scratch, kind, package, version, target, extra=(), digest=None):
        self.updates = scratch / 'updates'
        self.updates.mkdir(mode=0o700, exist_ok=True)
        self.session = Path(tempfile.mkdtemp(prefix='session-', dir=self.updates))
        script = self.session / 'install.sh'
        script.write_text(helper(kind)); script.chmod(0o700)
        self.parent = subprocess.Popen(['/bin/sleep', '300'])
        self.helper = subprocess.Popen(['/bin/sh', str(script), str(self.session), str(self.parent.pid), str(package),
                                        digest or sha256(package), version, str(target), str(self.updates), *extra],
                                       cwd=self.updates, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        wait_for(lambda: (self.session / 'ready').is_file() or self.helper.poll() is not None, 10, 'the helper to acknowledge')
        if not (self.session / 'ready').is_file():
            raise AssertionError('helper exited before acknowledging: ' + self.helper.communicate()[1])

    def finish(self, commit, between=None):
        """CMRemote's final close (commit '0'/'1') or a canceled quit/crash (None), then its exit."""
        time.sleep(0.3)
        check(self.helper.poll() is None, 'helper must wait while CMRemote runs')
        if between: between()
        if commit is not None: (self.session / 'commit').write_text(commit)
        self.parent.kill(); self.parent.wait()
        self.helper.wait(timeout=180)
        check(not self.session.exists(), 'helper left its session behind')
        result = self.updates / 'result.json'
        return json.loads(result.read_text()) if result.exists() else None


def release_module():
    spec = importlib.util.spec_from_file_location('release', ROOT / 'scripts/release.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


class MacFixtures:
    def __init__(self, scratch):
        self.scratch = scratch
        self.arch = 'arm64' if platform.machine() == 'arm64' else 'x86_64'
        self.release = release_module()

    def bundle(self, path, version, identifier=IDENTIFIER, marker=None):
        """A minimal ad-hoc-signed CMRemote.app whose executable records each launch."""
        macos = path / 'Contents/MacOS'; macos.mkdir(parents=True)
        (path / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleIdentifier=identifier, CFBundleShortVersionString=version, CFBundleExecutable='RemoteManager',
            CFBundleName='CMRemote', CFBundlePackageType='APPL', LSUIElement=True)))
        source = self.scratch / ('launch-' + version + '.c')
        record = json.dumps(str(marker or '/dev/null'))
        source.write_text('#include <stdio.h>\nint main(void) { FILE* f = fopen(' + record + ', "a");'
                          ' if (f) { fputs("launched ' + version + '\\n", f); fclose(f); } return 0; }\n')
        subprocess.run(['cc', '-arch', self.arch, '-o', str(macos / 'RemoteManager'), str(source)], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', str(path)], check=True, capture_output=True)
        return path

    def image(self, name, version, identifier=IDENTIFIER, marker=None):
        """A disk image laid out exactly like a published release (scripts/release.py)."""
        work = Path(tempfile.mkdtemp(dir=self.scratch))
        bundle = self.bundle(work / 'CMRemote.app', version, identifier, marker)
        return self.release.macos_dmg(bundle, self.scratch / 'downloads' / name)

    def installed(self, case):
        folder = self.scratch / case / 'Applications'
        folder.mkdir(parents=True)
        target = self.bundle(folder / 'CMRemote.app', OLD)
        (folder / 'Other.app').mkdir(); (folder / 'Other.app/keep').write_text('unrelated application')
        (folder / 'notes.txt').write_text('unrelated file')
        return folder, target


def version_of(bundle):
    return plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())['CFBundleShortVersionString']


def mac_cases(scratch):
    fx = MacFixtures(scratch)
    marker = scratch / 'launches.txt'
    good = fx.image('CMRemote-2.0.0-macos.dmg', NEW, marker=marker)
    results = []

    def attempt(case, package, extra=None, digest=None, **kwargs):
        folder, target = fx.installed(case)
        before = {k: v for k, v in tree(folder).items() if not k.startswith('CMRemote.app')}
        run = Attempt(scratch / case, 'macos', package, NEW, target, extra or (IDENTIFIER, fx.arch), digest)
        result = run.finish(**kwargs)
        after = {k: v for k, v in tree(folder).items() if not k.startswith('CMRemote.app')}
        check(before == after, case + ': files beside the application changed: ' + str(sorted(set(before) ^ set(after))))
        check(not (run.updates / 'install.lock').exists(), case + ': install lock left behind')
        return result, target, run

    def failed(case, result, target, text):
        check(result and result['status'] == 'failed' and text in result['message'], case + ': expected failure "' + text + '", got ' + repr(result))
        check(version_of(target) == OLD, case + ': the previous version must remain')
        results.append(case)

    # Installs, removes the package and pending record, and relaunches only when asked.
    pending = None
    def seed():
        nonlocal pending
        pending = scratch / 'install/updates/pending.json'; pending.write_text('{}')
    result, target, run = attempt('install', good, commit='1', between=seed)
    check(result == {'schema': 1, 'status': 'installed', 'version': NEW, 'message': 'CMRemote 2.0.0 was installed.'}, 'install: ' + repr(result))
    check(version_of(target) == NEW, 'install: bundle not replaced')
    check(not good.exists() and not pending.exists(), 'install: package or pending record kept')
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(target)], check=True)
    wait_for(lambda: marker.exists() and 'launched 2.0.0' in marker.read_text(), 30, 'the new version to be relaunched')
    results.append('install + relaunch')

    good = fx.image('CMRemote-2.0.0-macos.dmg', NEW, marker=marker)
    marker.unlink()
    result, target, _ = attempt('quiet', good, commit='0')
    check(result['status'] == 'installed' and version_of(target) == NEW, 'quiet: ' + repr(result))
    time.sleep(2); check(not marker.exists(), 'quiet: relaunched although not asked')
    results.append('install without relaunch')

    good = fx.image('CMRemote-2.0.0-macos.dmg', NEW, marker=marker)
    result, target, _ = attempt('canceled', good, commit=None)
    check(result is None and version_of(target) == OLD and good.exists(), 'canceled: changed something without a commit')
    results.append('canceled quit / crash before commit')

    failed('identifier', *attempt('identifier', fx.image('other.dmg', NEW, identifier='com.example.Other'), commit='1')[:2], 'different application')
    failed('version', *attempt('version', fx.image('stale.dmg', '1.5.0'), commit='1')[:2], 'does not contain CMRemote 2.0.0')
    failed('architecture', *attempt('architecture', good, extra=(IDENTIFIER, 'ppc'), commit='1')[:2], 'does not run on ppc')
    failed('checksum', *attempt('checksum', good, digest='0' * 64, commit='1')[:2], 'changed after it was verified')
    failed('not an image', *attempt('not an image', scratch / 'launch-1.0.0.c', commit='1')[:2], 'Cannot open the update disk image')

    folder, target = fx.installed('read-only')
    run = Attempt(scratch / 'read-only', 'macos', good, NEW, target, (IDENTIFIER, fx.arch))
    folder.chmod(0o555)
    try: result = run.finish('1')
    finally: folder.chmod(0o755)
    failed('read-only folder', result, target, 'Cannot copy the new version')
    check(sorted(p.name for p in folder.iterdir()) == ['CMRemote.app', 'Other.app', 'notes.txt'], 'read-only: leftovers')

    folder, target = fx.installed('locked')
    run = Attempt(scratch / 'locked', 'macos', good, NEW, target, (IDENTIFIER, fx.arch))
    (run.updates / 'install.lock').mkdir()
    result = run.finish('1')
    failed('concurrent install', result, target, 'Another update is being installed')
    check((run.updates / 'install.lock').is_dir(), 'locked: removed another helper\'s lock')
    return results


def elf_fixture(scratch, payload, marker):
    """An executable ELF carrying the AppImage type-2 marker (bytes 8-10 are ignored by the loader)."""
    source = scratch / 'appimage.c'
    source.write_text('#include <stdio.h>\nint main(void) { FILE* f = fopen(' + json.dumps(str(marker)) + ', "a");'
                      ' if (f) { fputs(' + json.dumps(payload + '\n') + ', f); fclose(f); } return 0; }\n')
    output = scratch / ('appimage-' + payload)
    if not shutil.which('cc') or subprocess.run(['cc', '-o', str(output), str(source)]).returncode: return None
    data = bytearray(output.read_bytes())
    if data[:4] != b'\x7fELF': return None
    data[8:11] = b'AI\x02'; output.write_bytes(bytes(data)); output.chmod(0o755)
    return output


def appimage_cases(scratch):
    marker = scratch / 'launches.txt'
    runnable = platform.system() == 'Linux'
    results = []
    executable = []

    def package(name, payload):
        path = scratch / 'downloads' / name; path.parent.mkdir(exist_ok=True)
        fixture = elf_fixture(scratch, payload, marker) if runnable else None
        executable.append(bool(fixture))
        if fixture: shutil.copy2(fixture, path)
        else: path.write_bytes(b'\x7fELF\x02\x01\x01\x00AI\x02' + payload.encode())
        return path

    def installed(case):
        folder = scratch / case / 'apps'; folder.mkdir(parents=True)
        target = folder / 'CMRemote.AppImage'; target.write_bytes(b'\x7fELF\x02\x01\x01\x00AI\x02old'); target.chmod(0o750)
        (folder / 'notes.txt').write_text('unrelated file')
        return folder, target

    def attempt(case, source, commit='1', digest=None):
        folder, target = installed(case)
        run = Attempt(scratch / case, 'appimage', source, NEW, target, digest=digest)
        result = run.finish(commit)
        check((folder / 'notes.txt').read_text() == 'unrelated file', case + ': unrelated file changed')
        check(sorted(p.name for p in folder.iterdir()) == ['CMRemote.AppImage', 'notes.txt'], case + ': leftovers ' + str(list(folder.iterdir())))
        return result, target

    new = package('CMRemote-2.0.0-linux-x86_64.appimage', 'new')
    expected = new.read_bytes()
    result, target = attempt('install', new)
    check(result['status'] == 'installed' and target.read_bytes() == expected, 'install: ' + repr(result))
    check(stat.S_IMODE(target.stat().st_mode) == 0o750, 'install: permissions not preserved: ' + oct(target.stat().st_mode))
    check(not new.exists(), 'install: package kept')
    if executable[0]:
        wait_for(lambda: marker.exists() and 'new' in marker.read_text(), 15, 'the new AppImage to be relaunched')
        results.append('install + relaunch (permissions preserved)')
    else: results.append('install (permissions preserved; relaunch not executable on this host)')

    new = package('CMRemote-2.0.0-linux-x86_64.appimage', 'new')
    result, target = attempt('canceled', new, commit=None)
    check(result is None and target.read_bytes().endswith(b'old') and new.exists(), 'canceled: changed something without a commit')
    results.append('canceled quit / crash before commit')

    for case, source, text, digest in (
            ('checksum', new, 'changed after it was verified', '0' * 64),
            ('not an appimage', scratch / 'appimage.c' if (scratch / 'appimage.c').exists() else Path(__file__), 'not an AppImage', None)):
        result, target = attempt(case, source, digest=digest)
        check(result['status'] == 'failed' and text in result['message'], case + ': ' + repr(result))
        check(target.read_bytes().endswith(b'old'), case + ': previous version replaced')
        results.append(case)

    folder, target = installed('link')
    real = folder / 'CMRemote.AppImage'; moved = scratch / 'link/real.AppImage'; real.rename(moved); real.symlink_to(moved)
    run = Attempt(scratch / 'link', 'appimage', new, NEW, real)
    result = run.finish('1')
    check(result['status'] == 'failed' and 'is missing or is a link' in result['message'] and moved.read_bytes().endswith(b'old'), 'link: ' + repr(result))
    results.append('link target refused')
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', choices=['macos', 'appimage', 'all'], default='all')
    parser.add_argument('--linux-image', help='Also run the AppImage helper inside this Linux image (e.g. remotemanager-dev:<id>-arm64)')
    parser.add_argument('--scratch', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    base = args.scratch or ROOT / 'build' / ('macos' if platform.system() == 'Darwin' else 'linux') / 'test-data'
    base.mkdir(parents=True, exist_ok=True)
    report = {}
    with tempfile.TemporaryDirectory(prefix='auto-update-', dir=base) as directory:
        scratch = Path(directory)
        if args.suite in ('macos', 'all') and platform.system() == 'Darwin':
            (scratch / 'macos').mkdir(); report['macos'] = mac_cases(scratch / 'macos')
        if args.suite in ('appimage', 'all'):
            (scratch / 'appimage').mkdir(); report['appimage (' + platform.system() + ' /bin/sh)'] = appimage_cases(scratch / 'appimage')
    if args.linux_image:
        context = os.environ.get('RM_DOCKER_CONTEXT', 'orbstack' if platform.system() == 'Darwin' else '')
        docker = ['docker'] + (['--context', context] if context else [])
        with tempfile.TemporaryDirectory(prefix='auto-update-linux-', dir=base) as directory:
            subprocess.run(docker + ['run', '--rm', '--network', 'none', '--user', str(os.getuid()) + ':' + str(os.getgid()),
                                     '-v', str(ROOT / 'app/update') + ':/work/app/update:ro',
                                     '-v', str(ROOT / 'scripts') + ':/work/scripts:ro', '-v', directory + ':/scratch',
                                     args.linux_image, 'python3', '/work/scripts/test-auto-update.py', '--suite', 'appimage', '--scratch', '/scratch'],
                           check=True)
        report['appimage (Linux container ' + args.linux_image + ')'] = 'passed'
    for suite, cases in report.items():
        print(suite + ':', cases if isinstance(cases, str) else ', '.join(cases))


if __name__ == '__main__':
    main()
