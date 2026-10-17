#!/usr/bin/env python3
"""Project-scoped Docker commands; never switch the caller's Docker context."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def configuration(arch=None):
    arch = arch or ('arm64' if platform.machine() in ('arm64', 'aarch64') else 'amd64')
    instance = hashlib.sha256(str(ROOT).encode()).hexdigest()[:12]
    env = dict(os.environ, RM_PROJECT_ID=instance, RM_ARCH=arch,
               LOCAL_UID=str(os.getuid()), LOCAL_GID=str(os.getgid()))
    context = os.environ.get('RM_DOCKER_CONTEXT', 'orbstack' if platform.system() == 'Darwin' else '')
    docker = ['docker'] + (['--context', context] if context else [])
    return docker, env, instance, arch


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('command', choices=['image', 'release-image', 'build', 'test', 'run', 'package', 'release', 'windows-image', 'windows',
                                       'shell', 'up', 'down', 'clean', 'env'])
    p.add_argument('--arch', choices=['arm64', 'amd64'])
    p.add_argument('--yes', action='store_true', help='allow project Docker cleanup; otherwise print targets')
    args, extra = p.parse_known_args()
    docker, env, instance, arch = configuration(args.arch)
    builder = 'remotemanager-' + instance + '-' + arch
    image = 'remotemanager-dev:' + instance + '-' + arch
    release_image = 'remotemanager-release:' + instance + '-' + arch
    windows_image = 'remotemanager-windows:' + instance + '-' + arch
    compose = docker + ['compose', '-f', str(ROOT / 'compose.yaml')]

    def run(command, **kwargs):
        return subprocess.run(command, cwd=ROOT, env=env, check=True, **kwargs)

    if args.command == 'env':
        print(json.dumps({k: env[k] for k in ('RM_PROJECT_ID', 'RM_ARCH', 'LOCAL_UID', 'LOCAL_GID')}, indent=2))
    elif args.command == 'image':
        if subprocess.run(docker + ['buildx', 'inspect', builder], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            run(docker + ['buildx', 'create', '--name', builder, '--driver', 'docker-container'])
        run(docker + ['buildx', 'build', '--builder', builder, '--target', 'dev', '--platform', 'linux/' + arch, '--load',
                     '--build-arg', 'LOCAL_UID=' + env['LOCAL_UID'], '--build-arg', 'LOCAL_GID=' + env['LOCAL_GID'],
                     '--label', 'org.remotemanager.instance=' + instance, '-t', image, '-f', 'Dockerfile', '.'])
    elif args.command == 'release-image':
        # The daemon's own builder, because the base is the locally loaded dev image.
        run(docker + ['build', '--platform', 'linux/' + arch,
                     '--build-arg', 'BASE=' + image, '--build-arg', 'LOCAL_UID=' + env['LOCAL_UID'],
                     '--build-arg', 'LOCAL_GID=' + env['LOCAL_GID'],
                     '--label', 'org.remotemanager.project=remotemanager',
                     '--label', 'org.remotemanager.instance=' + instance, '-t', release_image,
                     '-f', 'Dockerfile.release', '.'])
    elif args.command == 'release':
        # Same mounts and environment as the dev service, with installer tooling on top.
        run(compose + ['run', '--rm', '--no-deps', '-e', 'APPIMAGE_EXTRACT_AND_RUN=1', 'release',
                       'python3', 'scripts/release.py', 'linux'] + extra)
    elif args.command == 'windows-image':
        run(docker + ['build', '--platform', 'linux/' + arch,
                     '--build-arg', 'LOCAL_UID=' + env['LOCAL_UID'], '--build-arg', 'LOCAL_GID=' + env['LOCAL_GID'],
                     '--label', 'org.remotemanager.instance=' + instance, '-t', windows_image,
                     '-f', 'Dockerfile.windows', '.'])
    elif args.command == 'windows':
        # Cross-compiles for Windows; extra arguments are the command (default: interactive shell).
        run(compose + ['run', '--rm', '--no-deps', 'windows'] + (extra or ['/bin/bash']))
    elif args.command in ('build', 'test', 'run', 'package'):
        run(compose + ['run', '--rm', '--no-deps', 'dev', 'python3', 'scripts/project.py', args.command] + extra)
    elif args.command == 'shell':
        run(compose + ['run', '--rm', '--no-deps', 'dev', '/bin/bash'] + extra)
    elif args.command == 'up':
        run(compose + ['up', '-d', '--no-build'])
    elif args.command == 'down':
        run(compose + ['down', '--remove-orphans'])
    elif args.command == 'clean':
        filters = ['--filter', 'label=org.remotemanager.project=remotemanager',
                   '--filter', 'label=org.remotemanager.instance=' + instance]
        containers = run(docker + ['ps', '-aq'] + filters, capture_output=True, text=True).stdout.split()
        images = run(docker + ['image', 'ls', '-q'] + filters, capture_output=True, text=True).stdout.split()
        builders = ['remotemanager-' + instance + '-' + a for a in ('arm64', 'amd64')]
        print('Project containers:', containers, '\nProject images:', images, '\nProject builders/caches:', builders)
        if not args.yes:
            print('Dry run. Add --yes to remove these resources. Source and bind-mounted data are preserved.')
            return
        if containers:
            run(docker + ['rm', '-f'] + containers)
        for image_id in sorted(set(images)):
            inspected = run(docker + ['image', 'inspect', image_id], capture_output=True, text=True)
            labels = json.loads(inspected.stdout)[0]['Config'].get('Labels') or {}
            if labels.get('org.remotemanager.instance') != instance or labels.get('org.remotemanager.project') != 'remotemanager':
                raise SystemExit('Refusing to remove an image without matching project labels')
            # Remove this project's tags without deleting another project's tag of the same image.
            tags = json.loads(inspected.stdout)[0].get('RepoTags') or []
            for tag in tags:
                if tag.startswith(('remotemanager-dev:' + instance + '-', 'remotemanager-rdp-test:' + instance + '-',
                                   'remotemanager-release:' + instance + '-', 'remotemanager-windows:' + instance + '-')):
                    run(docker + ['image', 'rm', tag])
        for builder in builders:
            if subprocess.run(docker + ['buildx', 'inspect', builder], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                run(docker + ['buildx', 'rm', builder])
        for a in ('arm64', 'amd64'):
            scoped = dict(env, RM_ARCH=a)
            subprocess.run(compose + ['down', '--remove-orphans'], cwd=ROOT, env=scoped, check=True)


if __name__ == '__main__':
    main()
