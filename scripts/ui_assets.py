#!/usr/bin/env python3
"""Validate and stage the pinned, assets-only mRemoteNG subset without upstream code."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM_URL = 'https://github.com/mRemoteNG/mRemoteNG.git'
UPSTREAM_COMMIT = '4ad1c794fe2389cc5ca0bb1d271f009f44f6e0e6'
LICENSE_WARNING = ('Per-image rights are unresolved. Aggregate upstream notices do not '
                   'establish the license of each image. The images are redistributed with CMRemote '
                   'under the terms of the upstream GPL v2 distribution, with its notices.')
PREFIX_COUNTS = {'/original': 47, '/connection-icons': 39}
REFERENCE_ENTRIES = {
    'reference/reconnect.png': 'cmenTabReconnect.Image',
    'reference/external-tools.png': 'cmenTabExternalApps.Image',
}


def validate_image(data, filename):
    """Check bounded standalone PNG/ICO structure; Qt tests perform actual decoding."""
    if not data or len(data) > 4 * 1024 * 1024:
        raise ValueError('Image byte count outside preview limits')
    if filename.endswith('.png'):
        if len(data) < 24 or data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
            raise ValueError('Not a standalone PNG')
        width, height = struct.unpack('>II', data[16:24])
        if not (0 < width <= 2048 and 0 < height <= 2048):
            raise ValueError('PNG dimensions outside preview limits')
    elif filename.endswith('.ico'):
        if len(data) < 6 or data[:4] != b'\x00\x00\x01\x00':
            raise ValueError('Not a standalone ICO')
        count = struct.unpack_from('<H', data, 4)[0]
        if not (0 < count <= 64) or len(data) < 6 + count * 16:
            raise ValueError('Invalid ICO directory')
        for i in range(count):
            size, offset = struct.unpack_from('<II', data, 6 + i * 16 + 8)
            if not size or offset < 6 + count * 16 or offset + size > len(data):
                raise ValueError('Invalid ICO image bounds')
    else:
        raise ValueError('Unsupported preview image format')


def _relative(value):
    if (not isinstance(value, str) or not value or '\\' in value
            or any(part in ('', '.', '..') for part in value.split('/'))
            or PurePosixPath(value).is_absolute() or ':' in value):
        raise ValueError('Unsafe asset path: ' + str(value))
    return value


def _no_symlinks(path):
    path = Path(os.path.abspath(path))
    for component in (*reversed(path.parents), path):
        if component.is_symlink():
            raise ValueError('Symlink in asset path: ' + str(component))
    return path


def _read(path, limit):
    path = _no_symlinks(path)
    if not path.is_file() or not stat.S_ISREG(path.stat().st_mode):
        raise ValueError('Missing or non-regular asset file: ' + str(path))
    if path.stat().st_size > limit:
        raise ValueError('Asset file exceeds size limit: ' + str(path))
    return path.read_bytes()


def _json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate asset manifest key: ' + key)
            result[key] = value
        return result
    try:
        return json.loads(data, object_pairs_hook=unique)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError('Invalid asset manifest JSON') from error


def _catalog(qrc_path, asset_root):
    data = _read(qrc_path, 128 * 1024)
    if b'<!DOCTYPE' in data or b'<!ENTITY' in data:
        raise ValueError('Resource catalog declarations are not allowed')
    try:
        root = ET.fromstring(data)
    except ET.ParseError as error:
        raise ValueError('Invalid resource catalog XML') from error
    if root.tag != 'RCC' or root.attrib:
        raise ValueError('Invalid resource catalog root')
    aliases, counts = {}, {}
    for group in root:
        prefix = group.get('prefix')
        if (group.tag != 'qresource' or set(group.attrib) != {'prefix'}
                or prefix not in PREFIX_COUNTS or prefix in counts):
            raise ValueError('Invalid or duplicate resource namespace')
        counts[prefix] = 0
        for entry in group:
            if (entry.tag != 'file' or set(entry.attrib) != {'alias'} or len(entry)
                    or not entry.text or '\\' in entry.text or Path(entry.text).is_absolute()):
                raise ValueError('Invalid resource catalog entry')
            alias = _relative(entry.get('alias'))
            if '/' in alias:
                raise ValueError('Resource aliases must be filenames')
            resource = ':' + prefix + '/' + alias
            if resource in aliases:
                raise ValueError('Duplicate resource alias: ' + resource)
            path = _no_symlinks(qrc_path.parent / entry.text)
            if entry.text != Path(os.path.relpath(path, qrc_path.parent)).as_posix():
                raise ValueError('Non-canonical resource catalog path: ' + resource)
            try:
                relative = path.relative_to(asset_root).as_posix()
            except ValueError as error:
                raise ValueError('Resource escapes curated asset tree: ' + resource) from error
            aliases[resource] = _relative(relative)
            counts[prefix] += 1
    if counts != PREFIX_COUNTS:
        raise ValueError('Resource namespace counts differ from reviewed subset')
    if (':/original/mRemoteNG_Icon.ico' in aliases
            or aliases.get(':/original/Connection.ico') != 'Icons/mRemoteNG.ico'
            or aliases.get(':/connection-icons/mRemoteNG.ico') != 'Icons/mRemoteNG.ico'):
        raise ValueError('Application-logo removal or connection-icon compatibility changed')
    return aliases


def _validated(project_root, asset_root, qrc_path):
    project_root = _no_symlinks(project_root)
    asset_root = _no_symlinks(asset_root or project_root / 'vendor/mremoteng-assets')
    qrc_path = _no_symlinks(qrc_path or project_root / 'app/ui/preview-icons.qrc')
    if not asset_root.is_dir():
        raise ValueError('Missing curated asset directory')
    manifest_bytes = _read(asset_root / 'manifest.json', 256 * 1024)
    manifest = _json(manifest_bytes)
    if (not isinstance(manifest, dict)
            or set(manifest) != {'schema_version', 'upstream', 'licensing_warning', 'assets', 'notices'}
            or manifest['schema_version'] != 1
            or manifest['upstream'] != {'url': UPSTREAM_URL, 'commit': UPSTREAM_COMMIT}
            or manifest['licensing_warning'] != LICENSE_WARNING
            or not isinstance(manifest['assets'], list) or not isinstance(manifest['notices'], list)):
        raise ValueError('Asset manifest differs from pinned source/license contract')
    files, expected_aliases = {'manifest.json': manifest_bytes}, {}
    references, icon_count, resource_count = set(), 0, 0
    records = [(record, True) for record in manifest['assets']]
    records += [(record, False) for record in manifest['notices']]
    for record, image in records:
        required = {'source', 'destination', 'sha256', 'aliases'} if image else {'source', 'destination', 'sha256'}
        if (not isinstance(record, dict) or set(record) not in (required, required | {'resource_entry'})
                or (not image and 'resource_entry' in record)):
            raise ValueError('Invalid asset manifest record')
        relative = _relative(record['destination'])
        if relative in files:
            raise ValueError('Duplicate asset destination: ' + relative)
        source = record['source']
        if image:
            _relative(source)
            if (not isinstance(record['aliases'], list)
                    or not all(isinstance(alias, str) for alias in record['aliases'])):
                raise ValueError('Invalid resource alias list')
            if relative in REFERENCE_ENTRIES:
                if (source != 'mRemoteNG/UI/Window/ConnectionWindow.resx'
                        or record.get('resource_entry') != REFERENCE_ENTRIES[relative] or record['aliases']):
                    raise ValueError('Invalid extracted reference provenance')
                references.add(relative)
            else:
                if 'resource_entry' in record or source != 'mRemoteNG/' + relative:
                    raise ValueError('Invalid standalone image provenance')
                category, _, filename = relative.partition('/')
                if '/' in filename or not filename:
                    raise ValueError('Invalid standalone image destination')
                if category == 'Icons' and filename.endswith('.ico'):
                    wanted = [':/connection-icons/' + filename]
                    if filename == 'mRemoteNG.ico':
                        wanted.append(':/original/Connection.ico')
                    icon_count += 1
                elif category == 'Resources' and filename.endswith('.png'):
                    wanted = [':/original/' + filename]
                    resource_count += 1
                else:
                    raise ValueError('Unexpected standalone image category')
                if sorted(record['aliases']) != sorted(wanted):
                    raise ValueError('Image aliases differ from reviewed namespaces')
            for alias in record['aliases']:
                if alias in expected_aliases:
                    raise ValueError('Duplicate manifest alias: ' + str(alias))
                expected_aliases[alias] = relative
        elif relative not in ('COPYING.txt', 'CREDITS.md', 'NOTICE.txt') or source != (
                None if relative == 'NOTICE.txt' else relative):
            raise ValueError('Invalid upstream/local notice provenance')
        digest = record['sha256']
        if not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest):
            raise ValueError('Invalid asset checksum: ' + relative)
        data = _read(asset_root / relative, 4 * 1024 * 1024 if image else 1024 * 1024)
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('Asset checksum mismatch: ' + relative)
        if image:
            validate_image(data, relative)
        elif not data.strip():
            raise ValueError('Empty asset notice: ' + relative)
        files[relative] = data
    if (references != set(REFERENCE_ENTRIES) or icon_count != 39 or resource_count != 46
            or {r['destination'] for r in manifest['notices']} != {'COPYING.txt', 'CREDITS.md', 'NOTICE.txt'}):
        raise ValueError('Incomplete curated asset/notice set')
    actual = set()
    allowed_directories = {str(parent) for name in files for parent in PurePosixPath(name).parents
                           if str(parent) != '.'}
    for directory, dirs, names in os.walk(asset_root, followlinks=False):
        for name in dirs + names:
            path = _no_symlinks(Path(directory) / name)
            relative = path.relative_to(asset_root).as_posix()
            if path.is_dir():
                if relative not in allowed_directories:
                    raise ValueError('Unexpected curated asset directory: ' + relative)
            elif not stat.S_ISREG(path.stat().st_mode):
                raise ValueError('Non-regular curated asset: ' + relative)
            else:
                actual.add(relative)
    if actual != set(files):
        raise ValueError('Curated asset files differ from manifest: ' + str(sorted(actual ^ set(files))))
    if _catalog(qrc_path, asset_root) != expected_aliases:
        raise ValueError('Resource catalog differs from manifest aliases/destinations')
    return manifest, files, asset_root


def validate_assets(project_root=ROOT, *, asset_root=None, qrc_path=None):
    """Return the checked manifest; reject extra files, unsafe paths and catalog drift."""
    return _validated(Path(project_root), asset_root, qrc_path)[0]


def stage_assets(destination, project_root=ROOT, *, include_images=True, asset_root=None, qrc_path=None):
    """Stage checked assets, notices and provenance; False stages notices/provenance only.

    The destination must be dedicated to this helper. Existing unexpected files or
    symlinks are rejected, never cleaned. Images keep their manifest-relative paths;
    preview callers set RM_PREVIEW_ASSET_DIR to destination / 'reference'.
    """
    manifest, files, source_root = _validated(Path(project_root), asset_root, qrc_path)
    destination = _no_symlinks(destination)
    if destination.is_relative_to(source_root) or source_root.is_relative_to(destination):
        raise ValueError('Asset staging destination overlaps source tree')
    if not include_images:
        files = {name: data for name, data in files.items()
                 if name == 'manifest.json' or name in {r['destination'] for r in manifest['notices']}}
    provenance = {'upstream': manifest['upstream'], 'licensing_warning': LICENSE_WARNING,
                  'images_staged': include_images, 'assets': manifest['assets'], 'notices': manifest['notices']}
    files['provenance.json'] = (json.dumps(provenance, indent=2) + '\n').encode()
    if destination.exists():
        if not destination.is_dir():
            raise ValueError('Asset staging destination is not a directory')
        expected_dirs = {str(parent) for name in files for parent in PurePosixPath(name).parents
                         if str(parent) != '.'}
        for directory, dirs, names in os.walk(destination, followlinks=False):
            for name in dirs + names:
                path = _no_symlinks(Path(directory) / name)
                relative = path.relative_to(destination).as_posix()
                if ((path.is_dir() and relative not in expected_dirs)
                        or (not path.is_dir() and (relative not in files or not path.is_file()))):
                    raise ValueError('Unexpected staged asset path: ' + relative)
    for relative, data in files.items():
        target = _no_symlinks(destination / relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        # Do not truncate a pre-existing hard link or follow a link during staging.
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0)
        if target.exists():
            if not stat.S_ISREG(target.stat().st_mode):
                raise ValueError('Non-regular staged asset: ' + relative)
            target.unlink()
        with os.fdopen(os.open(target, flags, 0o644), 'wb') as output:
            output.write(data)
    return manifest


if __name__ == '__main__':
    manifest = validate_assets()
    print('Validated', len(manifest['assets']), 'images,',
          sum(len(record['aliases']) for record in manifest['assets']), 'resource aliases and',
          len(manifest['notices']), 'notices at', UPSTREAM_COMMIT)
