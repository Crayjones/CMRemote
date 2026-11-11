#!/usr/bin/env python3
"""Manual VeNCrypt test server: an owned TigerVNC container on loopback ports, one per security type.

  up      build the vnc-fixture image, start it and print the ports and credentials
  check   run an independent handshake through ServerInit on every port
  client  run CMRemote's own VNC client (vnc_fixture_test) against every port
  down    remove the container

The password is generated on first use and kept in .dev/vnc-fixture/ (git-ignored) with the
fixture CA, so it survives restarts. It never appears in Docker arguments.
"""
import argparse
import json
from pathlib import Path
import secrets
import string
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from docker import ROOT, configuration
sys.path.insert(0, str(ROOT / 'tests/vnc-fixture'))
from ports import PORTS, USER

STATE = ROOT / '.dev/vnc-fixture'
NAMES = {256: 'Plain', 258: 'TLSVnc', 259: 'TLSPlain', 261: 'X509Vnc', 262: 'X509Plain'}


def password():
    path = STATE / 'password'
    if not path.is_file():
        STATE.mkdir(parents=True, exist_ok=True)
        # VncAuth uses eight bytes, so the same password works for every type.
        path.write_text(''.join(secrets.choice(string.ascii_letters + string.digits) for _ in range(8)))
        path.chmod(0o600)
    return path.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['up', 'check', 'client', 'down'])
    parser.add_argument('cases', nargs='*', help='client only: QtTest functions to run (default: all)')
    parser.add_argument('--port', type=int, default=15900, help='first host port (default 15900; 5900 is often macOS Screen Sharing)')
    args = parser.parse_args()
    docker, env, instance, arch = configuration()
    name = 'remotemanager-vnc-fixture-' + instance + '-' + arch
    image = 'remotemanager-vnc-test:' + instance + '-' + arch
    secret_env = dict(env, RM_VNC_TEST_PASSWORD=password())

    if args.command == 'client':
        # CMRemote's own LibVNCClient worker against every port; build vnc_fixture_test first.
        sys.path.insert(0, str(ROOT / 'scripts'))
        from env import environment
        client_env = dict(environment(), RM_VNC_FIXTURE_PASSWORD=password(), RM_VNC_FIXTURE_CA=str(STATE / 'ca.pem'),
                          RM_VNC_FIXTURE_PORT=str(args.port), QT_QPA_PLATFORM='offscreen')
        binary = Path(client_env['RM_BUILD_DIR']) / 'app/vnc_fixture_test'
        if not binary.is_file():
            raise SystemExit('Build it first: python3 scripts/project.py build --target vnc_fixture_test')
        raise SystemExit(subprocess.run([str(binary), *args.cases], env=client_env).returncode)
    if args.command == 'down':
        subprocess.run(docker + ['rm', '-f', name], env=env, check=True, capture_output=True)
        print('Removed', name)
    elif args.command == 'up':
        labels = ['--label', 'org.remotemanager.project=remotemanager', '--label', 'org.remotemanager.instance=' + instance]
        subprocess.run(docker + ['build', '--platform', 'linux/' + arch, '--target', 'vnc-fixture', '-f', 'Dockerfile', '-t', image]
                       + labels + ['.'], cwd=ROOT, env=env, check=True)
        subprocess.run(docker + ['rm', '-f', name], env=env, capture_output=True)
        published = [argument for port, *_ in PORTS for argument in ('-p', '127.0.0.1:%d:%d' % (args.port + port - 5900, port))]
        subprocess.run(docker + ['run', '-d', '--init', '--name', name, '-e', 'RM_VNC_TEST_PASSWORD'] + labels + published + [image],
                       env=secret_env, check=True, stdout=subprocess.DEVNULL)
        for _ in range(50):
            ca = subprocess.run(docker + ['exec', name, 'cat', '/fixture/tls/ca.pem'], env=env, capture_output=True)
            if ca.returncode == 0 and ca.stdout: break
            time.sleep(0.2)
        else:
            raise SystemExit('Fixture did not create its CA; see: docker logs ' + name)
        (STATE / 'ca.pem').write_bytes(ca.stdout)
        print('\nVNC fixture %s on 127.0.0.1 (user %s, password %s)' % (name, USER, password()))
        print('CA for X509* types: %s (certificate is for localhost / 127.0.0.1)\n' % (STATE / 'ca.pem'))
        for port, types, _, subtype in PORTS:
            label = types if ',' in types or subtype is None else '%s (VeNCrypt %d)' % (types, subtype)
            print('  %5d  %s' % (args.port + port - 5900, label))
        print('\nRun "python3 scripts/vnc-fixture.py check" to verify every port.')
    else:
        result = subprocess.run(docker + ['exec', '-e', 'RM_VNC_TEST_PASSWORD', name, 'python3', '/fixture/probe.py'],
                                env=secret_env, capture_output=True, text=True)
        if not result.stdout:
            raise SystemExit(result.stderr.strip() or 'Fixture is not running; use "up" first')
        for line in result.stdout.splitlines():
            report = json.loads(line)
            if report['ok']:
                subtype = {port: subtype for port, _, _, subtype in PORTS}[report['port']]
                detail = ('subtypes %s, chose %s, %s' % (report['subtypes'], NAMES[subtype], report.get('tls', 'no TLS'))
                          if subtype else 'VncAuth')
                print('  ok    %5d  %s -> "%s" %dx%d' % (args.port + report['port'] - 5900, detail, report['name'], *report['size']))
            else:
                print('  FAIL  %5d  %s' % (args.port + report['port'] - 5900, report['error']))
        raise SystemExit(result.returncode)


if __name__ == '__main__': main()
