#!/usr/bin/env python3
"""Build an isolated Qt-only UI preview with the existing contained SDK."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
from env import ROOT, environment
from ui_assets import stage_assets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['build', 'test', 'run', 'capture'])
    parser.add_argument('--offscreen', action='store_true', help='No native window required')
    parser.add_argument('--smoke', action='store_true', help='Exit after a short run')
    parser.add_argument('--menu', choices=['tree', 'session', 'group', 'config', 'notifications', 'external-tools'],
                        help='Open this context menu for interaction or capture')
    args = parser.parse_args()
    if args.menu and args.command not in ('run', 'capture'):
        parser.error('--menu requires run or capture')
    env = environment()
    build = Path(env['RM_BUILD_DIR']) / 'ui-preview'
    temp = build / 'test-data'
    temp.mkdir(parents=True, exist_ok=True)
    env.update(TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp),
               RM_PREVIEW_ASSET_DIR=str(build / 'assets/mremoteng-assets/reference'))
    if args.offscreen or args.command == 'test':
        env['QT_QPA_PLATFORM'] = 'offscreen'
    stage_assets(build / 'assets/mremoteng-assets', project_root=ROOT)

    def run(*command):
        subprocess.run([str(x) for x in command], cwd=ROOT, env=env, check=True)

    run('cmake', '-S', ROOT / 'app/preview', '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug')
    run('cmake', '--build', build, '--parallel', str(min(os.cpu_count() or 2, 8)))
    if args.command == 'test':
        run('ctest', '--test-dir', build, '--output-on-failure')
    elif args.command in ('run', 'capture'):
        command = [build / 'UiPreview']
        if args.smoke:
            command.append('--smoke')
        if args.menu:
            command += ['--menu', args.menu]
        if args.command == 'capture':
            suffix = ('-' + args.menu) if args.menu else ''
            filename = ('preview-offscreen' if args.offscreen else 'preview-native') + suffix + '.png'
            command += ['--screenshot', build / filename]
        run(*command)
        if args.command == 'capture':
            print('Capture: ' + str(command[-1]))


if __name__ == '__main__':
    main()
