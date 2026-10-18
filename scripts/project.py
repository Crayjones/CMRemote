#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
from env import ROOT, environment


def fetch_notices(platform):
    import importlib.util, json
    spec = importlib.util.spec_from_file_location('audit', ROOT / 'scripts/verify-dependencies.py')
    audit = importlib.util.module_from_spec(spec); spec.loader.exec_module(audit)
    cache = ROOT / '.dev' / platform / 'downloads'; cache.mkdir(parents=True, exist_ok=True)
    for key, artifact in json.loads((ROOT / 'dependencies.lock.json').read_text())['artifacts'].items():
        if key.startswith('rust-notice-'): audit.fetch(artifact, cache)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['build','test','run','package'])
    parser.add_argument('--mock-only', action='store_true')
    parser.add_argument('--target', action='append', default=[], help='Build selected CMake targets only (build command)')
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--demo', action='store_true')
    parser.add_argument('--artifact-name', help='Package under this name (package command)')
    parser.add_argument('--update-channel', choices=['github'], help='Mark a published release package that updates itself (Release workflow only)')
    parser.add_argument('--release', action='store_true', help='Optimized Release build in its own build directory (published installers use this)')
    args = parser.parse_args()
    if args.update_channel and args.command != 'package': parser.error('--update-channel requires package')
    if args.target and (args.command != 'build' or any(value.startswith('-') or not value.replace('_', '').replace('-', '').isascii() or not value.replace('_', '').replace('-', '').isalnum() for value in args.target)):
        parser.error('--target requires build and a plain CMake target name')
    env = environment(); build = Path(env['RM_BUILD_DIR']) / ('app-mock' if args.mock_only else 'app-release' if args.release else 'app')
    env['RM_APP_BUILD_DIR'] = str(build)
    def run(*command): subprocess.run([str(x) for x in command],cwd=ROOT,env=env,check=True)
    print('Build:', build, flush=True)
    # Every consumer refreshes this tree, including its CMake-managed Rust dependencies.
    if env['RM_PLATFORM'] == 'windows' and args.command in ('test', 'run'):
        parser.error('Windows cross builds support build and package only; run the result on Windows')
    run('cmake','-S',ROOT,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=' + ('Release' if args.release else 'Debug'),
        '-DRM_WITH_RDP=' + ('OFF' if args.mock_only else 'ON'),
        # Headless test executables cannot run in the Linux cross container.
        # Release packages skip test executables; tests run against the Debug tree.
        *(['-DRM_BUILD_TESTS=OFF'] if env['RM_PLATFORM'] == 'windows' or (args.release and args.command == 'package') else []))
    targets = ['--target', *args.target] if args.target else []
    run('cmake','--build',build,'--parallel',str(min(os.cpu_count() or 2,8)), *targets)
    if args.command == 'test':
        # Contain Rust tempfile test data as well as Qt temporary files.
        temp = Path(env['RM_BUILD_DIR']) / 'test-data'; temp.mkdir(parents=True,exist_ok=True)
        env['TMPDIR'] = str(temp); env['TMP'] = str(temp); env['TEMP'] = str(temp)
        run('cargo','test','--locked')
        run(sys.executable,'-m','unittest','discover','-s','tests','-p','test_*.py','-v')
        run('ctest','--test-dir',build,'--output-on-failure')
    elif args.command == 'run':
        exe = build / ('RemoteManager.app/Contents/MacOS/RemoteManager' if env['RM_PLATFORM'] == 'macos' else 'RemoteManager')
        command = [exe,'--dev-root',ROOT]
        if args.smoke: command += ['--smoke']
        if args.demo: command += ['--demo']
        run(*command)
    elif args.command == 'package':
        # The offline notice audit includes locked non-native target dependencies and
        # upstream-only license files; container images do not keep the bootstrap cache.
        run('cargo','fetch','--locked')
        fetch_notices(env['RM_PLATFORM'])
        run(sys.executable,ROOT / 'scripts/package.py',*(['--artifact-name',args.artifact_name] if args.artifact_name else []),
            *(['--update-channel',args.update_channel] if args.update_channel else []))


if __name__ == '__main__': main()
