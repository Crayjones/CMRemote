#!/usr/bin/env python3
"""Test embedded FreeRDP against owned, temporary, loopback-only xrdp fixtures."""
import json
import argparse
import importlib.util
from pathlib import Path
import secrets
import socket
import subprocess
import time
import sys
sys.dont_write_bytecode = True
from docker import ROOT, configuration
from env import environment


def checked_output(output, *secrets):
    if any(secret and secret in output for secret in secrets):
        raise RuntimeError('Secret appeared in fixture logs; output suppressed')
    return output


def fixture_inputs(case, first, second=None):
    def valid(values):
        return (values and isinstance(values.get('port'), int) and 1 <= values['port'] <= 65535 and
                all(isinstance(values.get(key), str) and (12 if key == 'master' and case in
                    ('configPasswordXmlThroughShell', 'multipleConnectionFilesThroughShell') else 1) <= len(values[key]) <= 4096 and
                    all(32 <= ord(char) <= 126 for char in values[key]) for key in ('password', 'master')))
    if not valid(first): raise ValueError('Invalid primary fixture inputs')
    if case == 'multipleConnectionFilesThroughShell':
        if not valid(second) or any(first[key] == second[key] for key in ('port', 'password', 'master')):
            raise ValueError('Independent secondary fixture inputs required')
    elif second is not None: raise ValueError('Secondary fixture only supported for multiple-file acceptance')
    result = {'RM_RDP_TEST_PASSWORD': first['password'], 'RM_XML_TEST_MASTER': first['master'], 'RM_RDP_TEST_PORT': str(first['port'])}
    if second:
        result.update(RM_RDP_TEST_PASSWORD_2=second['password'], RM_XML_TEST_MASTER_2=second['master'], RM_RDP_TEST_PORT_2=str(second['port']))
    return result


def xml_input_keys(case, secondary=False):
    if case == 'configPasswordXmlThroughShell' and not secondary: return {'a', 'b'}, set()
    if case == 'multipleConnectionFilesThroughShell':
        return ({'b', 'd', 'f'}, {'a', 'c', 'e', 'g'}) if secondary else ({'a', 'c', 'e', 'g'}, {'b', 'd', 'f'})
    raise ValueError('Unknown XML server input oracle')


def xml_input_check(case, secondary=False):
    required, forbidden = xml_input_keys(case, secondary)
    return ("from pathlib import Path; s=Path('/tmp/rm-input').read_text(); lines=[line.split() for line in s.splitlines()]; "
            "keys={line[1].lower() for line in lines if line[0]=='2' and len(line)==4}; "
            f"assert {required!r} <= keys and not ({forbidden!r} & keys), 'Wrong fixture key ownership'; "
            "points=[tuple(map(int, line[-2:])) for line in lines if line[0]=='4']; "
            "assert any(abs(x-400)<=3 and abs(y-300)<=3 for x,y in points), 'Missing center click'; " +
            ("assert any(abs(x-200)<=3 and abs(y-150)<=3 for x,y in points), 'Missing restart/secondary click'; "
             if case == 'configPasswordXmlThroughShell' or secondary else '') +
            "print('Server verified XML authentication/restart and keyboard/mouse ownership')")


def cocoa_client(case, env, password, master, *secondary_secrets, target='rdp_integration'):
    spec = importlib.util.spec_from_file_location('cocoa_runner', ROOT / 'scripts/test-cocoa.py')
    runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
    return runner.run_cocoa(target, [case], env, (password, master, *secondary_secrets))


def linux_client(case, platform, docker, env, instance, arch, name, values, target='rdp_integration'):
    spec = importlib.util.spec_from_file_location('native_runner', ROOT / 'scripts/test-native.py')
    runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
    return runner.container_client(target, [case], platform, docker, env, instance, arch,
                                   name, values)


def session_features_check(case):
    if case == 'remoteCursorsThroughShell':
        return ("from pathlib import Path; names=set(Path('/tmp/rm-cursors').read_text().splitlines()); "
                "assert {'hand','text','resize','crosshair','busy'} <= names; "
                "print('Server verified owned cursor regions')")
    if case == 'bidirectionalTextClipboardThroughShell':
        return ("from pathlib import Path; assert Path('/tmp/rm-session-paste').read_text() == 'host-session-clipboard\\nemoji: 😀'; "
                "events=Path('/tmp/rm-input').read_text(); assert 'F6' in events and 'F7' in events and 'F8' in events; "
                "print('Server verified Unicode paste and copy/disabled-copy input')")
    if case == 'savedClipboardSettingStartsSharingThroughShell':
        return ("from pathlib import Path; assert Path('/tmp/rm-session-paste').read_text() == 'host-session-clipboard\\nemoji: 😀'; "
                "print('Server verified paste from the saved Clipboard setting')")
    raise ValueError('Unknown session feature oracle')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['offscreen', 'cocoa', 'xcb', 'wayland'], default='offscreen')
    parser.add_argument('--target', choices=['rdp_integration', 'rdp_session_integration'], default='rdp_integration')
    parser.add_argument('cases', nargs='*')
    args = parser.parse_args()
    if args.platform == 'cocoa' and sys.platform != 'darwin': parser.error('Cocoa requires macOS')
    docker, env, instance, arch = configuration()
    image = 'remotemanager-rdp-test:' + instance + '-' + arch
    name = 'remotemanager-rdp-test-' + instance + '-' + arch
    builder = 'remotemanager-' + instance + '-' + arch
    labels = ['--label', 'org.remotemanager.project=remotemanager', '--label', 'org.remotemanager.instance=' + instance]
    subprocess.run(docker + ['buildx', 'build', '--builder', builder, '--load', '--platform', 'linux/' + arch,
                            '--target', 'rdp-fixture', '-f', 'Dockerfile', '-t', image] + labels + ['.'], cwd=ROOT, env=env, check=True)
    native_linux = args.platform in ('xcb', 'wayland')
    targets = ['--target', args.target] if args.target == 'rdp_session_integration' else []
    subprocess.run(['python3', 'scripts/docker.py' if native_linux else 'scripts/project.py', 'build', *targets], cwd=ROOT, check=True)
    client_env = environment()
    build = ROOT / 'build/linux' / arch if native_linux else Path(client_env['RM_BUILD_DIR'])
    temp = build / 'test-data'
    temp.mkdir(parents=True, exist_ok=True)
    client_env.update(QT_QPA_PLATFORM=args.platform, TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    exe = build / 'app' / args.target
    listing = docker + ['compose', '-f', str(ROOT / 'compose.yaml'), 'run', '--rm', '--no-deps', 'dev',
        'python3', 'scripts/env.py', 'build/linux/' + arch + '/app/' + args.target, '-functions'] if native_linux else [str(exe), '-functions']
    available = subprocess.check_output(listing, env=dict(env if native_linux else client_env, QT_QPA_PLATFORM='offscreen'), text=True).splitlines()
    cases = args.cases or [entry.removesuffix('()') for entry in available if entry.endswith('()')]
    if not cases or any(case + '()' not in available for case in cases):
        raise RuntimeError('Select an RDP integration test function')
    outputs = []
    for case in cases:
        # Each test owns a fresh Xorg/Tk/clipboard state; transport closure does not reset xrdp's desktop.
        password = secrets.token_urlsafe(24)
        master = secrets.token_urlsafe(32)
        multiple = case == 'multipleConnectionFilesThroughShell'
        password2, master2 = (secrets.token_urlsafe(24), secrets.token_urlsafe(32)) if multiple else ('', '')
        all_secrets = (password, master, password2, master2)
        env['RM_RDP_TEST_PASSWORD'] = password
        owned = []
        secondary_name = name + '-secondary'
        try:
            ports = ['-p', '127.0.0.1::3389'] + (['-p', '127.0.0.1::3390'] if multiple else [])
            subprocess.run(docker + ['run', '-d', '--rm', '--name', name] + ports + ['-e', 'RM_RDP_TEST_PASSWORD'] + labels + [image],
                           env=env, check=True, stdout=subprocess.DEVNULL)
            owned.append(name)
            if args.target == 'rdp_session_integration':
                subprocess.run(docker + ['exec', name, 'python3', '-c',
                    "from pathlib import Path; Path('/tmp/rm-session-features').touch()"], env=env, check=True)
            if multiple:
                subprocess.run(docker + ['run', '-d', '--rm', '--name', secondary_name, '--network', 'container:' + name,
                    '-e', 'RM_RDP_TEST_PASSWORD', '-e', 'RM_RDP_TEST_SECONDARY=1'] + labels + [image],
                    env=dict(env, RM_RDP_TEST_PASSWORD=password2), check=True, stdout=subprocess.DEVNULL)
                owned.append(secondary_name)
            packages = subprocess.check_output(docker + ['exec', name, 'python3', '-c',
                "from pathlib import Path; print(Path('/fixture-packages.txt').read_text())"], env=env, text=True)
            (build / 'rdp-fixture-packages.txt').write_text(checked_output(packages, *all_secrets))
            details = json.loads(subprocess.check_output(docker + ['inspect', name], env=env))[0]
            port = int(details['NetworkSettings']['Ports']['3389/tcp'][0]['HostPort'])
            port2 = int(details['NetworkSettings']['Ports']['3390/tcp'][0]['HostPort']) if multiple else None
            for endpoint in ([port, port2] if multiple else [port]):
                for _ in range(100):
                    try:
                        with socket.create_connection(('127.0.0.1', endpoint), timeout=1): break
                    except OSError: time.sleep(0.1)
                else: raise RuntimeError('Owned xrdp server did not listen')
            first = dict(port=3389 if native_linux else port, password=password, master=master)
            second = dict(port=3390 if native_linux else port2, password=password2, master=master2) if multiple else None
            values = fixture_inputs(case, first, second)
            if args.target == 'rdp_session_integration': values.pop('RM_XML_TEST_MASTER')
            run_env = dict(client_env, **values)
            if native_linux:
                result = linux_client(case, args.platform, docker, env, instance, arch, name, values, target=args.target)
            elif args.platform == 'cocoa': result = cocoa_client(case, run_env, *(all_secrets if multiple else (password, master)), target=args.target)
            else: result = subprocess.run([str(exe), case], env=run_env, capture_output=True, text=True, timeout=240)
            output = checked_output(result.stdout + result.stderr, *all_secrets)
            print(output)
            outputs.append(output)
            (build / 'rdp-integration.log').write_text('\n'.join(outputs))
            if result.returncode:
                diagnostics = subprocess.run(docker + ['exec', name, 'python3', '-c',
                    "from pathlib import Path; [print(str(p) + '\\n' + p.read_text(errors='replace')) for p in [Path('/var/log/xrdp.log'), Path('/var/log/xrdp-sesman.log')]]"],
                    env=env, capture_output=True, text=True)
                if args.target == 'rdp_session_integration':
                    feature = subprocess.run(docker + ['exec', name, 'python3', '-c',
                        "from pathlib import Path; [print(str(p) + '\\n' + p.read_text()) for p in [Path('/tmp/rm-input'), Path('/tmp/rm-session-paste'), Path('/tmp/rm-cursors')] if p.is_file()]"],
                        env=env, capture_output=True, text=True)
                    diagnostics.stdout += feature.stdout + feature.stderr
                (build / 'rdp-server.log').write_text(checked_output(diagnostics.stdout + diagnostics.stderr, *all_secrets))
                if multiple:
                    diagnostics = subprocess.run(docker + ['exec', secondary_name, 'python3', '-c',
                        "from pathlib import Path; [print(str(p) + '\\n' + p.read_text(errors='replace')) for p in [Path('/var/log/xrdp.log'), Path('/var/log/xrdp-sesman.log')]]"],
                        env=env, capture_output=True, text=True)
                    (build / 'rdp-server-secondary.log').write_text(checked_output(diagnostics.stdout + diagnostics.stderr, *all_secrets))
                raise RuntimeError('RDP integration client failed; contained server diagnostics saved')
            if args.target == 'rdp_session_integration':
                events = subprocess.run(docker + ['exec', name, 'python3', '-c', session_features_check(case)],
                                        env=env, capture_output=True, text=True)
                print(checked_output(events.stdout + events.stderr, *all_secrets))
                if events.returncode: raise RuntimeError('Session feature server oracle failed')
            if case in {'configPasswordXmlThroughShell', 'multipleConnectionFilesThroughShell'}:
                for index, fixture in enumerate(owned):
                    events = subprocess.run(docker + ['exec', fixture, 'python3', '-c', xml_input_check(case, secondary=index == 1)],
                                            env=env, capture_output=True, text=True)
                    print(checked_output(events.stdout + events.stderr, *all_secrets))
                    if events.returncode: raise RuntimeError('XML server-side input ownership verification failed')
            if case in {'liveFramesInputClipboardResizeAndReconnect', 'savedConnectionThroughShell', 'quickConnectThroughShell', 'savedXmlCredentialsThroughShell', 'protectedConnectionsThroughShell', 'fixedResolutionPreservesInputMapping', 'fullscreenThroughShell', 'dragSessionBetweenWindowsThroughShell'}:
                check = "from pathlib import Path; s=Path('/tmp/rm-input').read_text(); assert 'a' in s and '4 ' in s, s; "
                if case == 'protectedConnectionsThroughShell':
                    # The first transport sends A/center; restart sends B/quarter.
                    check += "assert 'b' in s; points=[tuple(map(int, line.split()[-2:])) for line in s.splitlines() if line.startswith('4 ')]; assert any(abs(x-200)<=3 and abs(y-150)<=3 for x,y in points), points; "
                if case == 'fixedResolutionPreservesInputMapping':
                    check += "points=[tuple(map(int, line.split()[-2:])) for line in s.splitlines() if line.startswith('4 ')]; assert all(any(abs(x-a)<=3 and abs(y-b)<=3 for x,y in points) for a,b in [(320,180),(960,540)]), points; "
                if case == 'liveFramesInputClipboardResizeAndReconnect':
                    check += "assert Path('/tmp/rm-clipboard').read_text() == 'client-clipboard-fixture'; "
                check += "print('Server verified keyboard/mouse for " + case + "')"
                events = subprocess.run(docker + ['exec', name, 'python3', '-c', check], env=env, capture_output=True, text=True)
                print(checked_output(events.stdout + events.stderr, password, master))
                if events.returncode: raise RuntimeError('Server-side input/clipboard verification failed')
        finally:
            for fixture in reversed(owned):
                subprocess.run(docker + ['rm', '-f', fixture], env=env, check=True, stdout=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
