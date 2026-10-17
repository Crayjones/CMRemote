import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[1]


def runner_module():
    spec = importlib.util.spec_from_file_location('ssh_runner', ROOT / 'scripts/test-ssh.py')
    runner = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(ROOT / 'scripts'))
    try: spec.loader.exec_module(runner)
    finally: sys.path.pop(0)
    return runner


class SshTools(unittest.TestCase):
    def test_ssh_fixture_requires_banner_and_ready_oracle(self):
        runner = runner_module()
        socket = MagicMock(); socket.__enter__.return_value.makefile.return_value.__enter__.return_value.readline.return_value = b'SSH-2.0-OpenSSH_fixture\r\n'
        response = MagicMock(); response.__enter__.return_value.read.return_value = b'{"auth":0}'
        with patch.object(runner.socket, 'create_connection', return_value=socket), patch.object(runner, 'urlopen', return_value=response):
            self.assertTrue(runner.fixture_ready(2200, 8022))
            for banner in (b'', b'HTTP/1.0 200 OK\r\n', b'SSH-2.0-' + b'x' * 250):
                socket.__enter__.return_value.makefile.return_value.__enter__.return_value.readline.return_value = banner
                self.assertFalse(runner.fixture_ready(2200, 8022))
            socket.__enter__.return_value.makefile.return_value.__enter__.return_value.readline.return_value = b'SSH-2.0-OpenSSH_fixture\r\n'
            for payload in (b'', b'{}', b'{"auth":-1}', b'{"auth":true}', b'{"auth":"0"}', b'x' * 4097):
                response.__enter__.return_value.read.return_value = payload
                self.assertFalse(runner.fixture_ready(2200, 8022))
        with patch.object(runner.socket, 'create_connection', side_effect=OSError):
            self.assertFalse(runner.fixture_ready(2200, 8022))

    def test_ssh_fixture_keeps_secrets_and_cleanup_contained(self):
        runner = runner_module(); before = dict(os.environ)
        base = ROOT / 'build' / ('macos' if sys.platform == 'darwin' else 'linux') / 'test-data'; base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            (Path(directory) / 'build/linux/arm64').mkdir(parents=True)
            calls = []; client_keys = []
            def run(command, **kwargs):
                calls.append((command, kwargs))
                if 'ssh_fixture_key' in command:
                    keys = Path(kwargs['env']['RM_SSH_KEY_DIRECTORY']); (keys / 'encrypted-key').write_text('synthetic-private-key')
                    (keys / 'authorized-key').write_text('ssh-ed25519 synthetic-public-key')
                return subprocess.CompletedProcess(command, 0, 'safe', '')
            def output(command, **kwargs):
                if 'inspect' in command:
                    return json.dumps([{'NetworkSettings': {'Ports': {'22/tcp': [{'HostPort': '2200'}], '8022/tcp': [{'HostPort': '8022'}]}}}])
                return 'synthetic-old-public-key' if 'previous.pub' in command[-1] else 'owned package inventory'
            def client(target, cases, platform, docker, env, instance, arch, name, values):
                client_keys.append(Path(directory) / Path(values['RM_SSH_KEY_PATH']).relative_to('/workspace'))
                self.assertEqual(values['RM_SSH_TEST_PORT'], '22'); self.assertEqual(values['RM_SSH_ORACLE_PORT'], '8022')
                self.assertEqual(values['RM_SSH_TEST_PASSWORD'], 'synthetic-password'); self.assertEqual(values['RM_SSH_TEST_PASSPHRASE'], 'synthetic-passphrase')
                return SimpleNamespace(returncode=0, stdout='PASS: owned acceptance', stderr='')
            native = SimpleNamespace(container_client=client, accepted_report=lambda _: True)
            with patch('sys.argv', ['test-ssh.py', '--platform', 'xcb', 'temporarySshThroughShell']), \
                 patch.object(runner, 'ROOT', Path(directory)), patch.object(runner, 'configuration', return_value=(['docker'], {'LOCAL_UID': '501', 'LOCAL_GID': '20'}, 'aabbccddeeff', 'arm64')), \
                 patch.object(runner, 'environment', return_value={'RM_BUILD_DIR': directory}), patch.object(runner.secrets, 'token_urlsafe', side_effect=['synthetic-password', 'synthetic-passphrase']), \
                 patch.object(runner, 'fixture_ready', side_effect=[False, True]), patch.object(runner.time, 'sleep'), \
                 patch.object(runner.subprocess, 'run', side_effect=run), patch.object(runner.subprocess, 'check_output', side_effect=output), \
                 patch.object(runner, 'runner', return_value=native), redirect_stdout(io.StringIO()) as printed:
                runner.main()
            starts = [command for command, _ in calls if '-d' in command]; removals = [command for command, _ in calls if 'rm' in command]
            self.assertEqual(len(starts), 1); self.assertEqual(len(removals), 1)
            self.assertEqual(removals[0][-1], starts[0][starts[0].index('--name') + 1])
            self.assertIn('127.0.0.1::22', starts[0]); self.assertIn('127.0.0.1::8022', starts[0])
            for command, _ in calls:
                for secret in ('synthetic-password', 'synthetic-passphrase', 'synthetic-private-key'):
                    self.assertNotIn(secret, ' '.join(command)); self.assertNotIn(secret, printed.getvalue())
                self.assertNotIn('prune', command)
            self.assertTrue(all(not path.exists() for path in client_keys))
        self.assertEqual(dict(os.environ), before)

    def test_ssh_fixture_output_never_exposes_secrets(self):
        runner = runner_module()
        for secret in ('synthetic-password', 'synthetic-passphrase', 'synthetic-private-key'):
            with self.assertRaisesRegex(RuntimeError, 'suppressed') as error:
                runner.checked_output('diagnostic ' + secret, (secret,))
            self.assertNotIn(secret, str(error.exception))
