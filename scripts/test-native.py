#!/usr/bin/env python3
"""Run Qt tests under an owned Linux X11 or Wayland display with real RHI checks."""
import argparse
import importlib.util
from contextlib import contextmanager
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import tempfile
import time
import uuid
sys.dont_write_bytecode = True
from env import ROOT, environment

TARGETS = ('qt_smoke', 'property_grid_test', 'document_shell_test', 'xml_storage_test',
           'connection_moves_test', 'multiple_files_test', 'password_editing_test',
           'rdp_integration', 'rdp_session_integration', 'ssh_integration', 'ssh_sessions_test', 'terminal_test', 'terminal_appearance_test', 'performance_client', 'sdl_comparator')


def checked_output(output, secrets):
    if any(secret and secret in output for secret in secrets):
        raise RuntimeError('Secret appeared in native test output; output suppressed')
    return output


def valid_password_line(value):
    return isinstance(value, str) and 1 < len(value) <= 4096 and value.endswith('\n') and \
        '\n' not in value[:-1] and '\r' not in value and '\0' not in value


def run_comparator(cases, env, temp):
    password = sys.stdin.readline(4097)
    if not valid_password_line(password): raise RuntimeError('SDL stdin password is missing or unbounded')
    prefix = ROOT / '.dev/linux/comparator/prefix'
    binary = prefix / 'bin/sdl-freerdp'
    if not binary.is_file(): raise RuntimeError('Build the contained Linux SDL comparator first')
    with tempfile.TemporaryDirectory(prefix='sdl-config-', dir=temp) as directory:
        native = dict(env, RM_COMPARATOR_CONFIG=directory, XDG_CONFIG_HOME=directory,
            XDG_CACHE_HOME=directory, XDG_DATA_HOME=directory,
            SDL_VIDEODRIVER={'xcb': 'x11', 'wayland': 'wayland'}[env['QT_QPA_PLATFORM']],
            LD_LIBRARY_PATH=str(prefix / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', ''))
        native.pop('RM_RDP_TEST_PASSWORD', None); native.pop('RM_XML_TEST_MASTER', None)
        result = subprocess.run([str(binary), *cases], env=native, input=password,
                                capture_output=True, text=True, timeout=180)
        report = checked_output(result.stdout + result.stderr, (password[:-1],))
    report += '\nRM_NATIVE_EXIT=' + str(result.returncode) + '\n'
    spec = importlib.util.spec_from_file_location('comparator_report', Path(__file__).with_name('test-cocoa.py'))
    checker = importlib.util.module_from_spec(spec); spec.loader.exec_module(checker)
    print(report, end='')
    return result.returncode if checker.successful_comparator_report(report, result.returncode) else 1


def container_client(target, cases, platform, docker, env, instance, arch, fixture, values, stdin=None):
    if target not in ('rdp_integration', 'rdp_session_integration', 'ssh_integration', 'performance_client', 'sdl_comparator') or platform not in ('xcb', 'wayland') or \
            not re.fullmatch('[0-9a-f]{12}', instance) or arch not in ('arm64', 'amd64') or \
            not re.fullmatch(r'remotemanager-(?:rdp-test-' + instance + '-' + arch +
                             r'|ssh-test-' + instance + '-' + arch + r'-[0-9a-f]{12}|performance-' + instance + r'-[0-9a-f]{12})', fixture):
        raise ValueError('Native clients require an owned fixture and Linux platform')
    allowed = ('RM_RDP_TEST_PASSWORD', 'RM_XML_TEST_MASTER', 'RM_RDP_TEST_PORT',
               'RM_RDP_TEST_PASSWORD_2', 'RM_XML_TEST_MASTER_2', 'RM_RDP_TEST_PORT_2',
               'RM_PERFORMANCE_WORKLOAD', 'RM_PERFORMANCE_POLICY', 'RM_SSH_TEST_PASSWORD', 'RM_SSH_TEST_PASSPHRASE',
               'RM_SSH_KEY_PATH', 'RM_SSH_TEST_PORT', 'RM_SSH_ORACLE_PORT', 'RM_SSH_OLD_KEY')
    if any(key not in allowed for key in values): raise ValueError('Unexpected native client environment')
    if target == 'sdl_comparator':
        if values.keys() - {'RM_PERFORMANCE_WORKLOAD'} or not valid_password_line(stdin):
            raise ValueError('SDL credentials require one bounded stdin line, not environment values')
    elif stdin is not None:
        raise ValueError('Only the owned SDL comparator accepts stdin')
    child_env = dict(env, RM_RDP_TEST_PORT='3389'); child_env.update(values)
    name = 'remotemanager-native-client-' + instance + '-' + uuid.uuid4().hex[:12]
    command = docker + ['create', '--name', name, '--network', 'container:' + fixture,
        '--label', 'org.remotemanager.project=remotemanager', '--label', 'org.remotemanager.instance=' + instance,
        '--user', env['LOCAL_UID'] + ':' + env['LOCAL_GID'], '--volume', str(ROOT) + ':/workspace']
    if stdin is not None: command += ['--interactive']
    for key in ('RM_RDP_TEST_PORT', *values): command += ['--env', key]
    command += ['--entrypoint', 'python3', 'remotemanager-dev:' + instance + '-' + arch,
                'scripts/test-native.py', '--platform', platform, target, '--', *cases]
    secrets = tuple(values.get(key, '') for key in ('RM_RDP_TEST_PASSWORD', 'RM_XML_TEST_MASTER', 'RM_RDP_TEST_PASSWORD_2', 'RM_XML_TEST_MASTER_2', 'RM_SSH_TEST_PASSWORD', 'RM_SSH_TEST_PASSPHRASE'))
    if stdin is not None: secrets += (stdin[:-1],)
    created = subprocess.run(command, cwd=ROOT, env=child_env, capture_output=True, text=True, timeout=60)
    if created.returncode:
        checked_output(created.stdout + created.stderr, secrets)
        raise RuntimeError('Owned native client could not be created')
    try:
        attach = ['--interactive'] if stdin is not None else []
        result = subprocess.run(docker + ['start', '--attach', *attach, name], cwd=ROOT, env=env,
                                input=stdin, capture_output=True, text=True, timeout=240)
        checked_output(result.stdout + result.stderr, secrets)
        return result
    finally:
        removed = subprocess.run(docker + ['rm', '-f', name], env=env, capture_output=True, text=True, timeout=30)
        checked_output(removed.stdout + removed.stderr, secrets)
        if removed.returncode: raise RuntimeError('Owned native client cleanup failed')


def accepted_report(report):
    totals = re.search(r'^Totals: (\d+) passed, (\d+) failed, (\d+) skipped,', report, re.M)
    finished = re.search(r'^\*+ Finished testing of .+ \*+$', report, re.M)
    failures = ('qfatal', 'qrhiwidget: no qrhi', 'qrhiwidget: qrhi is not supported', 'unreleased resources')
    return bool(totals and int(totals[1]) > 0 and int(totals[2]) == 0 and finished
                and not any(failure in report.lower() for failure in failures))


def join_display(child):
    if child.poll() is None: child.terminate()
    try: child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        child.kill(); child.wait(timeout=5)


@contextmanager
def native_display(platform, env, build):
    build = Path(build).resolve()
    if platform not in ('xcb', 'wayland') or not build.is_relative_to((ROOT / 'build/linux').resolve()):
        raise RuntimeError('Native Linux displays require xcb/wayland and contained Linux output')
    base = build / 'native-tests'; base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=platform + '-', dir=base) as directory:
        runtime = Path(directory); runtime.chmod(0o700)
        test_env = dict(env, QT_QPA_PLATFORM=platform, XDG_RUNTIME_DIR=str(runtime), QT_RHI_LEAK_CHECK='1')
        test_env.pop('DISPLAY', None); test_env.pop('WAYLAND_DISPLAY', None)
        server_env = {key: value for key, value in test_env.items() if key not in ('RM_RDP_TEST_PASSWORD', 'RM_XML_TEST_MASTER', 'RM_RDP_TEST_PASSWORD_2', 'RM_XML_TEST_MASTER_2')}
        child = None; reader = writer = None
        with (runtime / 'display.log').open('wb') as log:
            try:
                if platform == 'xcb':
                    reader, writer = os.pipe()
                    child = subprocess.Popen(['Xvfb', '-displayfd', str(writer), '-screen', '0', '1280x1024x24',
                        '-nolisten', 'tcp', '-noreset'], env=server_env, stdout=log, stderr=log, pass_fds=(writer,))
                    os.close(writer); writer = None
                    deadline = time.monotonic() + 15; display = b''
                    while b'\n' not in display:
                        if child.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError('Owned Xvfb did not become ready')
                        if select.select([reader], [], [], 0.05)[0]: display += os.read(reader, 32)
                        if len(display) > 16: raise RuntimeError('Owned Xvfb returned an invalid display')
                    if not re.fullmatch(rb'\d+\n', display): raise RuntimeError('Owned Xvfb returned an invalid display')
                    test_env['DISPLAY'] = ':' + display.strip().decode('ascii')
                else:
                    socket = 'wayland-owned'
                    child = subprocess.Popen(['weston', '--backend=headless-backend.so', '--renderer=gl',
                        '--socket=' + socket, '--idle-time=0', '--no-config', '--width=1280', '--height=1024'],
                        env=server_env, stdout=log, stderr=log)
                    deadline = time.monotonic() + 15
                    while not (runtime / socket).exists():
                        if child.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError('Owned Weston GL compositor did not become ready')
                        time.sleep(0.05)
                    test_env['WAYLAND_DISPLAY'] = socket
                yield test_env
            finally:
                if reader is not None: os.close(reader)
                if writer is not None: os.close(writer)
                if child is not None: join_display(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['xcb', 'wayland'], required=True)
    parser.add_argument('target', choices=TARGETS)
    parser.add_argument('cases', nargs='*')
    args = parser.parse_args()
    if sys.platform != 'linux': parser.error('Native Linux execution requires Linux; use the project container')
    env = environment(); build = Path(env['RM_BUILD_DIR'])
    temp = build / 'test-data'; temp.mkdir(parents=True, exist_ok=True)
    env.update(TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    binary = build / 'app' / args.target
    if args.target != 'sdl_comparator' and not binary.is_file():
        raise RuntimeError('Build the Linux tests with scripts/project.py build first')
    secrets = tuple(env.get(key, '') for key in ('RM_RDP_TEST_PASSWORD', 'RM_XML_TEST_MASTER', 'RM_RDP_TEST_PASSWORD_2', 'RM_XML_TEST_MASTER_2', 'RM_SSH_TEST_PASSWORD', 'RM_SSH_TEST_PASSPHRASE'))
    with native_display(args.platform, env, build) as native:
        if args.target == 'sdl_comparator': return run_comparator(args.cases, native, temp)
        result = subprocess.run([str(binary), *args.cases], env=native, capture_output=True, text=True, timeout=180)
        report = checked_output(result.stdout + result.stderr, secrets)
    print(report, end='')
    return 0 if result.returncode == 0 and accepted_report(report) else 1


if __name__ == '__main__':
    raise SystemExit(main())
