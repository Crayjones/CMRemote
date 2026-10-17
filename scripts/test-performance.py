#!/usr/bin/env python3
"""Capture owned app/SDL baselines; missing native or clean-motion evidence stays blocked."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import uuid
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
WORKLOADS = ('idle', 'sparse', 'overlap', 'scroll', 'full', 'input')


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def checked(output, values):
    if len(output) > 2 * 1024 * 1024 or any(value and value in output for value in values):
        raise RuntimeError('Performance output rejected; output suppressed')
    return output


def result(output, secret_values):
    output = checked(output, secret_values)
    marker = 'RM_PERFORMANCE_RESULT='
    lines = [line.split(marker, 1)[1].strip() for line in output.splitlines() if marker in line]
    if len(lines) != 1: raise RuntimeError('Performance client produced no unique numeric report')
    try:
        report = json.loads(lines[0])
        if set(report) != {'schema', 'workload', 'width', 'height', 'dpr', 'trace', 'pixels',
                           'window_width', 'window_height', 'pixel_width', 'pixel_height'}:
            raise ValueError('Unexpected report field')
        if type(report['schema']) is not int or report['schema'] != 1 or type(report['workload']) is not int or report['workload'] not in range(6):
            raise ValueError('Invalid workload')
        for key in ('width', 'height', 'window_width', 'window_height', 'pixel_width', 'pixel_height'):
            if type(report[key]) is not int or not 200 <= report[key] <= 8192: raise ValueError('Invalid geometry')
        if report['width'] * report['height'] > 16 * 1024 * 1024: raise ValueError('Invalid area')
        if type(report['dpr']) not in (int, float) or not 0 < report['dpr'] <= 8: raise ValueError('Invalid DPR')
        trace = report['trace']
        if type(trace) is not dict or type(trace.get('schema')) is not int: raise ValueError('Invalid trace object')
        fields = {'schema', 'generation', 'dropped', 'codecs', 'stages'}
        if trace.get('schema') == 2: fields.add('submission_gaps')
        if set(trace) != fields or trace['schema'] not in (1, 2): raise ValueError('Invalid trace schema')
        if trace['schema'] == 2:
            gaps = trace['submission_gaps']
            if len(gaps) != 7 or any(type(v) is not int or not 0 <= v <= 2**64 - 1 for v in gaps) or \
                    gaps[0] >= 4096 or gaps[5] != 50000000 or gaps[6] > gaps[0] or \
                    not gaps[1] <= gaps[2] <= gaps[3] <= gaps[4]:
                raise ValueError('Invalid submission gaps')
        for key in ('generation', 'dropped'):
            if type(trace[key]) is not int or not 0 <= trace[key] <= 2**64 - 1: raise ValueError('Invalid counter')
        if len(trace['stages']) != 8 or len(trace['codecs']) > 13: raise ValueError('Invalid trace bound')
        for i, row in enumerate(trace['stages']):
            if len(row) != 14 or row[0] != i or any(type(v) is not int or not 0 <= v <= 2**64 - 1 for v in row):
                raise ValueError('Invalid stage row')
        if any(type(v) is not int or v not in range(13) for v in trace['codecs']): raise ValueError('Invalid codec')
        if len(report['pixels']) > 4096 or any(type(v) is not int or not 0 <= v <= 0xffffff for v in report['pixels']):
            raise ValueError('Invalid synthetic pixel bound')
    except (ValueError, TypeError, KeyError, OverflowError):
        raise RuntimeError('Performance report is not bounded numeric data') from None
    return report


def app_freerdp_description(prefix, frozen):
    if frozen: return 'Release; OpenH264/FFmpeg disabled; frozen runtime'
    header = (prefix / 'include/freerdp3/freerdp/config.h').read_text()
    features = set(re.findall(r'^#define (WITH_[A-Z0-9_]+)(?:\s|$)', header, re.MULTILINE))
    if features & {'WITH_FFMPEG', 'WITH_SWSCALE', 'WITH_MEDIA_FOUNDATION', 'WITH_MEDIACODEC',
                   'WITH_VAAPI', 'WITH_VIDEOTOOLBOX'}:
        raise RuntimeError('App decoder configuration differs from the audited software-only build')
    if {'WITH_OPENH264', 'WITH_GFX_H264', 'WITH_OPENH264_LOADING'} <= features:
        return 'Release; OpenH264 loaded at runtime; software-only'
    if features & {'WITH_OPENH264', 'WITH_GFX_H264', 'WITH_OPENH264_LOADING'}:
        raise RuntimeError('App decoder feature macros are inconsistent')
    return 'Release; OpenH264/FFmpeg disabled'


def matching(app, sdl, platform):
    blockers = []
    if platform == 'offscreen': blockers.append('offscreen is not native presentation evidence')
    if any(app[key] != sdl[key] for key in ('width', 'height', 'dpr')):
        blockers.append('remote geometry or DPR differs')
    if any(app[key] != sdl[key] for key in ('window_width', 'window_height', 'pixel_width', 'pixel_height')):
        blockers.append('native window or pixel geometry differs')
    if not app['trace']['codecs'] or not sdl['trace']['codecs'] or \
            0 in app['trace']['codecs'] or sorted(app['trace']['codecs']) != sorted(sdl['trace']['codecs']):
        blockers.append('observed codec missing or different')
    if app['trace']['dropped'] or sdl['trace']['dropped']: blockers.append('bounded trace overflowed')
    return blockers


def comparator_arguments(port, fingerprint):
    if type(port) is not int or not 0 < port <= 65535 or not re.fullmatch('[0-9a-f]{64}', fingerprint):
        raise ValueError('Invalid owned comparator endpoint or certificate fingerprint')
    return ['/v:127.0.0.1:' + str(port), '/u:rdpfixture', '/d:', '/from-stdin:force',
            '/cert:fingerprint:sha256:' + fingerprint, '/size:1024x768', '/bpp:32',
            '/network:lan', '-gfx', '-rfx', '-nsc', '-dynamic-resolution', '-clipboard',
            '/cache:bitmap:off', '+wallpaper', '+themes', '+fonts', '/log-level:ERROR',
            '/t:RemoteManager owned comparator']


def linux_client(platform, docker, env, instance, arch, name, values):
    runner = load('performance_native', ROOT / 'scripts/test-native.py')
    return runner.container_client('performance_client', ['ownedWorkload'], platform,
                                   docker, env, instance, arch, name, values)


def linux_comparator(platform, docker, env, instance, arch, name, cases, values, stdin):
    runner = load('comparator_native', ROOT / 'scripts/test-native.py')
    return runner.container_client('sdl_comparator', cases, platform,
                                   docker, env, instance, arch, name, values, stdin=stdin)


def linux_freerdp_description(docker, env):
    code = "import sys,runpy; from pathlib import Path; sys.path.insert(0,'scripts'); from env import environment; print(runpy.run_path('scripts/test-performance.py')['app_freerdp_description'](Path(environment()['RM_NATIVE_PREFIX']),False))"
    description = subprocess.check_output(docker + ['compose', '-f', str(ROOT / 'compose.yaml'),
        'run', '--rm', '--no-deps', 'dev', 'python3', '-c', code], env=env, text=True).strip()
    if description not in ('Release; OpenH264 loaded at runtime; software-only', 'Release; OpenH264/FFmpeg disabled'):
        raise RuntimeError('Linux decoder configuration could not be verified')
    return description


def capture(client, args, docker, env, instance, arch, image, labels, build, model, cocoa, environment):
    name = 'remotemanager-performance-' + instance + '-' + uuid.uuid4().hex[:12]
    password = secrets.token_urlsafe(24)
    owned_env = dict(env, RM_RDP_TEST_PASSWORD=password)
    started = False
    try:
        subprocess.run(docker + ['run', '-d', '--rm', '--name', name, '-p', '127.0.0.1::3389',
            '-e', 'RM_RDP_TEST_PASSWORD'] + labels + [image], env=owned_env, check=True, stdout=subprocess.DEVNULL)
        started = True
        details = json.loads(subprocess.check_output(docker + ['inspect', name], env=env))[0]
        port = int(details['NetworkSettings']['Ports']['3389/tcp'][0]['HostPort'])
        workload = WORKLOADS.index(args.workload)
        control = "from pathlib import Path; Path('/tmp/rm-performance-control.json').write_text(" + repr(json.dumps({'workload': workload})) + ")"
        subprocess.run(docker + ['exec', name, 'python3', '-c', control], env=env, check=True)
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=1): break
            except OSError: time.sleep(0.1)
        else: raise RuntimeError('Owned performance fixture did not listen')
        client_env = dict(environment(), RM_RDP_TEST_PORT=str(port), RM_PERFORMANCE_WORKLOAD=str(workload),
            RM_PERFORMANCE_POLICY=args.policy,
            QT_QPA_PLATFORM=args.platform, TMPDIR=str(build / 'test-data'), TMP=str(build / 'test-data'), TEMP=str(build / 'test-data'))
        if client == 'app':
            client_env['RM_RDP_TEST_PASSWORD'] = password
            if args.platform in ('xcb', 'wayland'):
                execution = linux_client(args.platform, docker, env, instance, arch, name,
                    {key: client_env[key] for key in ('RM_RDP_TEST_PASSWORD', 'RM_PERFORMANCE_POLICY', 'RM_PERFORMANCE_WORKLOAD')})
            elif args.platform == 'cocoa':
                execution = cocoa.run_cocoa('performance_client', ['ownedWorkload'], client_env, (password,),
                    save_report=False, baseline=args.replay_baseline)
            else:
                execution = subprocess.run([str(build / 'app/performance_client'), 'ownedWorkload'],
                    env=client_env, capture_output=True, text=True, timeout=60)
        else:
            if args.platform == 'offscreen': raise RuntimeError('SDL comparator requires a native window platform')
            certificate = subprocess.check_output(docker + ['exec', name, 'python3', '-c',
                "import hashlib,ssl; from pathlib import Path; print(hashlib.sha256(ssl.PEM_cert_to_DER_cert(Path('/etc/xrdp/cert.pem').read_text())).hexdigest())"], env=env, text=True)
            native_linux = args.platform in ('xcb', 'wayland')
            command = comparator_arguments(3389 if native_linux else port, checked(certificate, (password,)).strip())
            if native_linux:
                execution = linux_comparator(args.platform, docker, env, instance, arch, name, command,
                    {'RM_PERFORMANCE_WORKLOAD': str(workload)}, stdin=password + '\n')
            else:
                binary = ROOT / '.dev' / client_env['RM_PLATFORM'] / 'comparator/prefix/bin/sdl-freerdp'
                with tempfile.TemporaryDirectory(prefix='comparator-', dir=build / 'test-data') as directory:
                    client_env.pop('RM_RDP_TEST_PASSWORD', None)
                    client_env.update(RM_COMPARATOR_CONFIG=directory, XDG_CONFIG_HOME=directory,
                        XDG_CACHE_HOME=directory, XDG_DATA_HOME=directory, SDL_VIDEODRIVER='cocoa')
                    execution = cocoa.run_cocoa('sdl_comparator', command, client_env, (password,), save_report=False,
                        comparator=binary, stdin=(password + '\n').encode())
        output = checked(execution.stdout + execution.stderr, (password,))
        succeeded = execution.returncode == 0
        if client == 'sdl' and args.platform != 'cocoa':
            succeeded = cocoa.successful_comparator_report(output, execution.returncode)
        if not succeeded:
            print('\n'.join(line for line in output.splitlines() if 'RM_PERFORMANCE_RESULT=' not in line))
            reports = sum('RM_PERFORMANCE_RESULT=' in line for line in output.splitlines())
            raise RuntimeError('Owned ' + client + ' performance client failed; numeric reports=' + str(reports))
        report = result(output, (password,))
        fixture_text = subprocess.check_output(docker + ['exec', name, 'python3', '-c',
            "from pathlib import Path; print(Path('/tmp/rm-performance-result.json').read_text())"], env=env, text=True)
        fixture = json.loads(checked(fixture_text, (password,)))
        if report['pixels'] != model.expected_pixels(workload, model.FRAMES, report['width'], report['height']):
            raise RuntimeError('Final synthetic desktop contains stale or incorrect pixels')
        if fixture['pixels'] != report['pixels'] or fixture['width'] != report['width'] or fixture['height'] != report['height']:
            raise RuntimeError('Independent fixture geometry/pixel oracle differs')
        if workload == 5:
            events = subprocess.check_output(docker + ['exec', name, 'python3', '-c',
                "from pathlib import Path; s=Path('/tmp/rm-input').read_text(); assert 'a' in s and '4 ' in s; print(1)"], env=env, text=True)
            if checked(events, (password,)).strip() != '1': raise RuntimeError('Independent input oracle failed')
        report.pop('pixels')
        return report, fixture['frames'], fixture['elapsed_ns'], password
    finally:
        if started:
            subprocess.run(docker + ['rm', '-f', name], env=env, check=True, stdout=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['cocoa', 'xcb', 'wayland', 'offscreen'], required=True)
    parser.add_argument('--workload', choices=WORKLOADS, required=True)
    parser.add_argument('--policy', choices=['legacy', 'gfx'], default='legacy')
    parser.add_argument('--app-baseline', action='store_true', help='Capture app-only evidence, not SDL parity')
    parser.add_argument('--replay-baseline', type=Path, help='Replay a verified frozen macOS app baseline')
    args = parser.parse_args()
    if args.replay_baseline and args.policy != 'legacy': parser.error('Frozen baseline is legacy-only')
    if args.policy == 'gfx' and not args.app_baseline: parser.error('The current SDL comparator SDK is decoder-less and legacy-only')
    if args.replay_baseline and args.platform != 'cocoa': parser.error('Frozen baseline replay currently requires Cocoa')
    if args.platform == 'cocoa' and sys.platform != 'darwin': parser.error('Cocoa requires macOS')
    if args.platform == 'offscreen' and not args.app_baseline: parser.error('Offscreen can capture only an app baseline')
    native_linux = args.platform in ('xcb', 'wayland')
    sys.path.insert(0, str(ROOT / 'scripts'))
    from docker import configuration
    from env import environment
    docker, env, instance, arch = configuration()
    model = load('performance_fixture', ROOT / 'tests/rdp-fixture/performance.py')
    cocoa = load('performance_cocoa', ROOT / 'scripts/test-cocoa.py')
    build = ROOT / 'build/linux' / arch if native_linux else Path(environment()['RM_BUILD_DIR'])
    (build / 'test-data').mkdir(parents=True, exist_ok=True)
    image = 'remotemanager-rdp-test:' + instance + '-' + arch
    labels = ['--label', 'org.remotemanager.project=remotemanager', '--label', 'org.remotemanager.instance=' + instance]
    subprocess.run(docker + ['buildx', 'build', '--builder', 'remotemanager-' + instance + '-' + arch,
        '--load', '--platform', 'linux/' + arch, '--target', 'rdp-fixture', '-f', 'Dockerfile', '-t', image] + labels + ['.'],
        cwd=ROOT, env=env, check=True)
    if not args.replay_baseline:
        script = 'scripts/docker.py' if native_linux else 'scripts/project.py'
        subprocess.run([sys.executable, str(ROOT / script), 'build'], cwd=ROOT, check=True)
    else:
        load('verified_replay', ROOT / 'scripts/performance-baseline.py').verify(args.replay_baseline, build)
    app, frames, elapsed, app_password = capture('app', args, docker, env, instance, arch, image, labels, build, model, cocoa, environment)
    blockers = ['clean-motion sampling during movement missing', 'scaled-output SDL pixel acceptance unresolved']
    if args.app_baseline: blockers.append('matched instrumented SDL baseline missing')
    else: blockers.append('client measurement intervals are not aligned to workload markers')
    if args.platform == 'offscreen': blockers.append('offscreen is not native presentation evidence')
    if not app['trace']['codecs'] or 0 in app['trace']['codecs']: blockers.append('observed codec missing')
    if app['trace']['dropped']: blockers.append('bounded trace overflowed')
    if args.policy == 'gfx': blockers.append('requested GFX policy is not evidence of negotiated GFX/AVC')
    report = {'schema': 1, 'platform': args.platform, 'policy': args.policy, 'freerdp': '3.32.1',
              'display': 'canonical one remote pixel per output pixel; verify measured geometry',
              'app_source': args.replay_baseline.name if args.replay_baseline else 'current build',
              'app': app, 'final_scene_oracle': True, 'window_pixel_oracle': args.platform != 'offscreen',
              'server_frames': frames, 'server_elapsed_ns': elapsed, 'blockers': blockers,
              'builds': {'app': 'Debug', 'app_freerdp': linux_freerdp_description(docker, env) if native_linux else
                         app_freerdp_description(Path(environment()['RM_NATIVE_PREFIX']), bool(args.replay_baseline)),
                         'sdl': 'Release; OpenH264/FFmpeg disabled; test-only QtCore trace'}}
    password_values = [app_password]
    if not args.app_baseline:
        try:
            if native_linux:
                bootstrap = docker + ['compose', '-f', str(ROOT / 'compose.yaml'), 'run', '--rm', '--no-deps',
                    'dev', 'python3', 'scripts/bootstrap-comparator.py', '--platform', 'linux']
                subprocess.run(bootstrap, cwd=ROOT, env=env, check=True)
            else:
                subprocess.run([sys.executable, str(ROOT / 'scripts/bootstrap-comparator.py'), '--platform',
                                environment()['RM_PLATFORM']], cwd=ROOT, check=True)
            sdl, sdl_frames, sdl_elapsed, sdl_password = capture('sdl', args, docker, env, instance, arch, image, labels, build, model, cocoa, environment)
            password_values.append(sdl_password)
            blockers.extend(matching(app, sdl, args.platform))
            report.update(sdl=sdl, sdl_server_frames=sdl_frames, sdl_server_elapsed_ns=sdl_elapsed)
        except (RuntimeError, subprocess.SubprocessError, OSError, ValueError) as error:
            blockers.append('owned SDL capture failed; matched baseline unavailable')
            print('Owned SDL comparison failed (' + type(error).__name__ + '); app evidence retained.')
    encoded = checked(json.dumps(report, indent=2), password_values)
    directory = build / 'performance'; directory.mkdir(exist_ok=True)
    path = directory / (args.policy + '-' + args.platform + '-' + args.workload + '-' + uuid.uuid4().hex + '.json')
    path.write_text(encoded); print(encoded); print('Baseline report:', path)
    return 0 if args.app_baseline else (2 if blockers else 0)


if __name__ == '__main__':
    raise SystemExit(main())
