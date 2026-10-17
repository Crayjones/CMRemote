#!/usr/bin/env python3
"""Production SSH acceptance against owned loopback-only OpenSSH/vim fixtures."""
import argparse
import importlib.util
import json
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import uuid
from urllib.request import urlopen
sys.dont_write_bytecode = True
from docker import ROOT, configuration
from env import environment

CASES = ('savedSshPasswordThroughShell', 'savedSshEncryptedKeyThroughShell', 'temporarySshThroughShell',
         'sshHostKeysAcceptedWithoutPromptThroughShell', 'sshShellVimUnicodeAndResizeThroughShell', 'sshIndependentSessionsAndCancellationThroughShell',
         'sshControlShortcutsThroughShell', 'sshSessionZoomThroughShell', 'sshScrollbackDragSelectionThroughShell', 'sshIndexedColorsThroughShell',
         'sshScpUploadThroughTransferDialog')


def checked_output(output, hidden):
    if any(value and value in output for value in hidden): raise RuntimeError('Secret appeared in SSH fixture output; report suppressed')
    return output


def fixture_ready(port, oracle):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=1) as connection:
            with connection.makefile('rb') as stream:
                banner = stream.readline(256)
            if not banner.startswith(b'SSH-2.0-') or not banner.endswith(b'\n'): return False
        with urlopen('http://127.0.0.1:' + str(oracle) + '/state', timeout=1) as response:
            payload = response.read(4097)
        if len(payload) > 4096: return False
        state = json.loads(payload)
        return isinstance(state, dict) and type(state.get('auth')) is int and state['auth'] >= 0
    except (OSError, ValueError):
        return False


def runner(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['offscreen', 'cocoa', 'xcb'], default='offscreen')
    parser.add_argument('cases', nargs='*'); args = parser.parse_args()
    cases = args.cases or CASES
    if any(case not in CASES for case in cases): parser.error('Select an SSH integration test function')
    if args.platform == 'cocoa' and sys.platform != 'darwin': parser.error('Cocoa requires macOS')
    docker, env, instance, arch = configuration(); native_linux = args.platform == 'xcb'
    image = 'remotemanager-ssh-test:' + instance + '-' + arch
    labels = ['--label', 'org.remotemanager.project=remotemanager', '--label', 'org.remotemanager.instance=' + instance]
    subprocess.run(docker + ['buildx', 'build', '--builder', 'remotemanager-' + instance + '-' + arch,
        '--load', '--platform', 'linux/' + arch, '--target', 'ssh-fixture', '-f', 'Dockerfile', '-t', image] + labels + ['.'], cwd=ROOT, env=env, check=True)
    subprocess.run(['python3', 'scripts/docker.py' if native_linux else 'scripts/project.py', 'build', '--target', 'ssh_integration'], cwd=ROOT, check=True)
    client = environment(); build = ROOT / 'build/linux' / arch if native_linux else Path(client['RM_BUILD_DIR'])
    temp = Path(client['RM_BUILD_DIR']) / 'test-data'; temp.mkdir(parents=True, exist_ok=True)
    outputs = []
    for case in cases:
        password, passphrase = secrets.token_urlsafe(24), secrets.token_urlsafe(24)
        name = 'remotemanager-ssh-test-' + instance + '-' + arch + '-' + uuid.uuid4().hex[:12]
        created = False
        with tempfile.TemporaryDirectory(prefix='ssh-fixture-', dir=temp) as directory:
            keys = Path(directory); secret_env = dict(client, RM_SSH_KEY_DIRECTORY=directory, RM_SSH_TEST_PASSPHRASE=passphrase)
            keygen = subprocess.run(['python3', 'scripts/env.py', 'cargo', 'run', '--locked', '-p', 'remotemanager-core', '--example', 'ssh_fixture_key'],
                cwd=ROOT, env=secret_env, capture_output=True, text=True, timeout=120)
            checked_output(keygen.stdout + keygen.stderr, (password, passphrase))
            if keygen.returncode: raise RuntimeError('Owned synthetic key generation failed')
            hidden = (password, passphrase, (keys / 'encrypted-key').read_text())
            fixture_env = dict(env, RM_SSH_TEST_PASSWORD=password, RM_SSH_AUTHORIZED_KEY=(keys / 'authorized-key').read_text())
            try:
                result = subprocess.run(docker + ['run', '-d', '--rm', '--name', name, '-p', '127.0.0.1::22', '-p', '127.0.0.1::8022',
                    '-e', 'RM_SSH_TEST_PASSWORD', '-e', 'RM_SSH_AUTHORIZED_KEY'] + labels + [image], env=fixture_env, capture_output=True, text=True, check=True)
                created = True; checked_output(result.stdout + result.stderr, hidden)
                details = json.loads(subprocess.check_output(docker + ['inspect', name], env=env))[0]
                ports = details['NetworkSettings']['Ports']; port, oracle = (int(ports[key][0]['HostPort']) for key in ('22/tcp', '8022/tcp'))
                for _ in range(100):
                    if fixture_ready(port, oracle): break
                    time.sleep(0.1)
                else: raise RuntimeError('Owned OpenSSH fixture did not become ready')
                old_key = subprocess.check_output(docker + ['exec', name, 'python3', '-c',
                    "from pathlib import Path; print(Path('/fixture/previous.pub').read_text().split()[1])"], env=env, text=True).strip()
                packages = subprocess.check_output(docker + ['exec', name, 'python3', '-c',
                    "from pathlib import Path; print(Path('/fixture-packages.txt').read_text())"], env=env, text=True)
                (build / 'ssh-fixture-packages.txt').write_text(checked_output(packages, hidden))
                key_path = str(keys / 'encrypted-key')
                if native_linux: key_path = '/workspace/' + str((keys / 'encrypted-key').relative_to(ROOT))
                values = {'RM_SSH_TEST_PASSWORD': password, 'RM_SSH_TEST_PASSPHRASE': passphrase, 'RM_SSH_KEY_PATH': key_path,
                          'RM_SSH_TEST_PORT': str(22 if native_linux else port), 'RM_SSH_ORACLE_PORT': str(8022 if native_linux else oracle), 'RM_SSH_OLD_KEY': old_key}
                run_env = dict(client, **values, QT_QPA_PLATFORM=args.platform, TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
                if native_linux:
                    result = runner('test-native').container_client('ssh_integration', [case], 'xcb', docker, env, instance, arch, name, values)
                elif args.platform == 'cocoa': result = runner('test-cocoa').run_cocoa('ssh_integration', [case], run_env, hidden)
                else: result = subprocess.run([str(build / 'app/ssh_integration'), case], env=run_env, capture_output=True, text=True, timeout=180)
                output = checked_output(result.stdout + result.stderr, hidden); print(output); outputs.append(output)
                (build / 'ssh-integration.log').write_text('\n'.join(outputs))
                if result.returncode or not runner('test-native').accepted_report(output): raise RuntimeError('SSH production acceptance failed')
            finally:
                if created:
                    removed = subprocess.run(docker + ['rm', '-f', name], env=env, capture_output=True, text=True)
                    checked_output(removed.stdout + removed.stderr, hidden)
                    if removed.returncode: raise RuntimeError('Owned SSH fixture cleanup failed')


if __name__ == '__main__': main()
