#!/usr/bin/env python3
"""Run shell UI tests as foreground macOS apps, with project-local data."""
import argparse
import importlib.util
import os
from pathlib import Path
import plistlib
import re
import select
import shlex
import subprocess
import sys
import threading
import uuid
sys.dont_write_bytecode = True
from env import environment

TARGETS = ('property_grid_test', 'qt_smoke', 'document_shell_test', 'xml_storage_test', 'connection_moves_test', 'multiple_files_test', 'password_editing_test', 'rdp_integration', 'rdp_session_integration', 'ssh_integration', 'ssh_sessions_test', 'terminal_test', 'terminal_appearance_test')


def successful_report(result):
    totals = re.search(r'^Totals: (\d+) passed, (\d+) failed, (\d+) skipped,', result, re.M)
    finished = re.search(r'^\*+ Finished testing of .+ \*+$', result, re.M)
    return bool(totals and int(totals[1]) > 0 and int(totals[2]) == 0
                and finished and 'QFATAL' not in result)


def successful_comparator_report(report, exit_code=None):
    lines = report.splitlines()
    exits = [line for line in lines if line.startswith('RM_NATIVE_EXIT=')]
    if exit_code is None:
        if len(exits) != 1 or exits[0] not in ('RM_NATIVE_EXIT=0', 'RM_NATIVE_EXIT=131'): return False
        exit_code = int(exits[0].split('=')[1])
    elif exits and exits != ['RM_NATIVE_EXIT=' + str(exit_code)]: return False
    if exit_code not in (0, 131): return False
    reports = [i for i, line in enumerate(lines) if line.startswith('RM_PERFORMANCE_RESULT=')]
    if len(reports) != 1: return False
    markers = ['RM_COMPARATOR_STOP_REQUESTED=1', 'RM_COMPARATOR_JOINED=1']
    if exit_code == 131:
        markers[1:1] = ['RM_COMPARATOR_ABORT_CLEAN=1', 'RM_COMPARATOR_CANCELLED=1']
    positions = reports.copy()
    for marker in markers:
        if lines.count(marker) != 1: return False
        positions.append(lines.index(marker))
    if exits: positions.append(lines.index(exits[0]))
    return positions == sorted(positions)


def checked_report(result, secrets):
    if any(secret and secret in result for secret in secrets):
        raise RuntimeError('Secret appeared in native test output; output suppressed')
    return result


def run_cocoa(target, cases, env, secrets=(), save_report=True, comparator=None, stdin=None, baseline=None):
    if sys.platform != 'darwin':
        raise RuntimeError('Cocoa tests require macOS')
    build = Path(env['RM_BUILD_DIR'])
    binary = build / 'app' / target
    if baseline is not None:
        if target != 'performance_client' or comparator is not None or stdin is not None:
            raise RuntimeError('Only the owned performance client supports frozen baseline replay')
        spec = importlib.util.spec_from_file_location('frozen_baseline', Path(__file__).with_name('performance-baseline.py'))
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        bundle = module.verify(Path(baseline), build)
        binary = bundle / 'Contents/MacOS/performance_client'
        env = dict(env, QT_PLUGIN_PATH=str(bundle / 'Contents/PlugIns'))
    if comparator is not None:
        allowed = Path(__file__).resolve().parents[1] / '.dev/macos/comparator/prefix/bin/sdl-freerdp'
        if Path(comparator).resolve() != allowed.resolve() or stdin is None or len(stdin) > 4096:
            raise RuntimeError('Native comparator must use the contained binary and bounded stdin')
        binary = allowed
    elif stdin is not None:
        raise RuntimeError('Only the contained comparator accepts stdin')
    if not binary.is_file():
        raise RuntimeError('Build the native tests with scripts/project.py build first')
    base = build / 'native-tests'
    base.mkdir(exist_ok=True)
    # Each invocation owns a fresh bundle/report. Never replace another launch's files.
    run = base / (target + '-' + uuid.uuid4().hex)
    bundle = run / 'Test.app'
    executable = bundle / 'Contents/MacOS' / target
    executable.parent.mkdir(parents=True)
    input_pipe = None; input_fd = None
    if comparator is None:
        executable.symlink_to(binary)
    else:
        # Only public paths enter this script. Credentials stay in a FIFO, never a file or argv.
        input_pipe = run / 'input.pipe'; os.mkfifo(input_pipe, 0o600)
        input_fd = os.open(input_pipe, os.O_RDWR | os.O_NONBLOCK)
        if os.write(input_fd, stdin) != len(stdin):
            os.close(input_fd); input_pipe.unlink()
            raise RuntimeError('Native comparator stdin was not delivered')
        executable.write_text('#!/bin/sh\n' + shlex.quote(str(binary)) + ' "$@" < ' +
                              shlex.quote(str(input_pipe)) +
                              '\nrc=$?\nprintf "\\nRM_NATIVE_EXIT=%s\\n" "$rc"\nexit "$rc"\n')
        executable.chmod(0o700)
    metadata = {'CFBundleExecutable': target,
                'CFBundleIdentifier': 'dev.remotemanager.test.' + target.replace('_', '-'),
                'CFBundleName': 'RemoteManager UI Test', 'CFBundlePackageType': 'APPL',
                'NSHighResolutionCapable': True}
    (bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps(metadata))
    temp = run / 'test-data'; temp.mkdir()
    env = dict(env, QT_QPA_PLATFORM='cocoa', TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    # LaunchServices does not return the app's exit code or stdout. A FIFO captures
    # QtTest and native diagnostics in memory, before any secret-bearing log is saved.
    fifo = run / 'output.pipe'; os.mkfifo(fifo, 0o600)
    reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
    keepalive = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
    stop = threading.Event(); chunks = []
    def drain():
        while True:
            ready, _, _ = select.select([reader], [], [], 0.05)
            if ready:
                chunk = os.read(reader, 65536)
                if chunk: chunks.append(chunk)
            elif stop.is_set():
                break
    collector = threading.Thread(target=drain, daemon=True); collector.start()
    command = ['/usr/bin/open', '-n', '-W', '--stdout', str(fifo), '--stderr', str(fifo)]
    for key in ('RM_BUILD_DIR', 'RM_PLATFORM', 'RM_QT_PREFIX', 'RM_NATIVE_PREFIX',
                'QT_PLUGIN_PATH', 'QT_LOGGING_RULES', 'QT_QPA_PLATFORM', 'QT_SCALE_FACTOR',
                'TMPDIR', 'TMP', 'TEMP', 'RM_RDP_TEST_PASSWORD', 'RM_XML_TEST_MASTER', 'RM_RDP_TEST_PORT',
                'RM_RDP_TEST_PASSWORD_2', 'RM_XML_TEST_MASTER_2', 'RM_RDP_TEST_PORT_2', 'RM_PERFORMANCE_WORKLOAD',
                'RM_PERFORMANCE_POLICY', 'RM_COMPARATOR_CONFIG', 'RM_SSH_TEST_PASSWORD', 'RM_SSH_TEST_PASSPHRASE',
                'RM_SSH_KEY_PATH', 'RM_SSH_TEST_PORT', 'RM_SSH_ORACLE_PORT', 'RM_SSH_OLD_KEY', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'SDL_VIDEODRIVER'):
        if key in env: command += ['--env', key + '=' + env[key]]
    command += [str(bundle), '--args', *cases]
    if comparator is None: command += ['-o', str(fifo) + ',txt']
    launch_error = ''
    try:
        result = subprocess.run(command, check=True, timeout=180, env=env, capture_output=True)
        chunks.append(result.stdout + result.stderr)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        launch_error = 'Native test launch failed or timed out.\n'
    finally:
        stop.set(); collector.join(); os.close(keepalive); os.close(reader); fifo.unlink()
        if input_fd is not None: os.close(input_fd); input_pipe.unlink()
    report = checked_report(launch_error + b''.join(chunks).decode(errors='replace'), secrets)
    suffix = ''
    if save_report:
        log = run / 'results.txt'; log.write_text(report); suffix = f'Report: {log}\n'
    succeeded = successful_report(report) if comparator is None else successful_comparator_report(report)
    return subprocess.CompletedProcess(command, 0 if not launch_error and succeeded else 1,
                                       stdout=report + suffix, stderr='')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=TARGETS)
    parser.add_argument('cases', nargs='*', help='QtTest case names; empty runs all')
    args = parser.parse_args()
    env = environment()
    secrets = tuple(env.get(key, '') for key in ('RM_RDP_TEST_PASSWORD', 'RM_XML_TEST_MASTER', 'RM_RDP_TEST_PASSWORD_2', 'RM_XML_TEST_MASTER_2'))
    try:
        result = run_cocoa(args.target, args.cases, env, secrets)
    except RuntimeError as error:
        print(str(error), file=sys.stderr); return 1
    print(result.stdout, end='')
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
