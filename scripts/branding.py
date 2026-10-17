#!/usr/bin/env python3
"""Generate the supplied logo's native icons on macOS, or verify them anywhere."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'app/assets/branding'
SOURCE_SHA256 = 'cec053a2e3b4297c26a6cd97acce309a7919da14a1ad48b6cef7df6391f5f70a'
SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
FAVICON_SIZES = (16, 32, 48)
# Supplied artwork's slate body (pixels) and fill; app icons recompose it on the macOS icon grid.
BODY = (226, 231, 803, 790)
BODY_RGB = (40, 46, 53)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def png_size(data):
    if len(data) < 33 or data[:8] != b'\x89PNG\r\n\x1a\n' or data[8:16] != b'\x00\x00\x00\rIHDR':
        raise ValueError('Invalid branding PNG')
    size = struct.unpack_from('>II', data, 16)
    if not all(0 < value <= 4096 for value in size):
        raise ValueError('Branding PNG dimensions outside limits')
    return size


def ico_bytes(images):
    offset = 6 + 16 * len(images)
    entries, payloads = [], []
    for size, data in images:
        if png_size(data) != (size, size) or not 0 < size <= 256:
            raise ValueError('ICO frame dimensions do not match PNG')
        entries.append(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        payloads.append(data)
        offset += len(data)
    return struct.pack('<HHH', 0, 1, len(images)) + b''.join(entries + payloads)


def validate_ico(data, sizes, assets, prefix='cmremote-'):
    if len(data) < 6 + 16 * len(sizes) or struct.unpack_from('<HHH', data) != (0, 1, len(sizes)):
        raise ValueError('Invalid branding ICO directory')
    offset = 6 + 16 * len(sizes)
    for index, size in enumerate(sizes):
        width, height, colors, reserved, planes, bits, length, start = struct.unpack_from('<BBBBHHII', data, 6 + 16 * index)
        expected = (assets / (prefix + str(size) + '.png')).read_bytes()
        if (width or 256, height or 256, colors, reserved, planes, bits, start, length) != \
                (size, size, 0, 0, 1, 32, offset, len(expected)) or data[start:start + length] != expected:
            raise ValueError('Branding ICO does not contain the expected PNG frame')
        offset += length
    if offset != len(data):
        raise ValueError('Unexpected branding ICO payload')


def validate_icns(data):
    if len(data) < 16 or data[:4] != b'icns' or struct.unpack_from('>I', data, 4)[0] != len(data):
        raise ValueError('Invalid branding ICNS')
    offset = 8
    chunks = set()
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError('Truncated branding ICNS chunk')
        kind, length = struct.unpack_from('>4sI', data, offset)
        if length < 8 or offset + length > len(data):
            raise ValueError('Invalid branding ICNS chunk length')
        chunks.add(kind)
        offset += length
    if not {b'ic07', b'ic08', b'ic09', b'ic10'}.issubset(chunks):
        raise ValueError('Branding ICNS is missing large icon representations')


def verify(assets=ASSETS):
    manifest = json.loads((assets / 'manifest.json').read_text())
    names = set(asset_names())
    if manifest.get('schema') != 1 or manifest.get('source_sha256') != SOURCE_SHA256 or set(manifest.get('files', {})) != names:
        raise ValueError('Branding manifest does not match the supplied artwork contract')
    for name, checksum in manifest['files'].items():
        path = assets / name
        if path.is_symlink() or not path.is_file() or digest(path.read_bytes()) != checksum:
            raise ValueError('Branding asset missing or changed: ' + name)
    source = (assets / 'source.png').read_bytes()
    if digest(source) != SOURCE_SHA256 or png_size(source) != (1254, 1254):
        raise ValueError('Branding source differs from the supplied logo')
    for size in SIZES:
        if png_size((assets / ('cmremote-' + str(size) + '.png')).read_bytes()) != (size, size):
            raise ValueError('Branding icon dimensions do not match filename')
    validate_ico((assets / 'cmremote.ico').read_bytes(), ICO_SIZES, assets)
    validate_ico((assets / 'favicon.ico').read_bytes(), FAVICON_SIZES, assets, 'favicon-')
    validate_icns((assets / 'cmremote.icns').read_bytes())
    return manifest


def asset_names():
    return (['source.png', 'cmremote.ico', 'favicon.ico', 'cmremote.icns'] + ['cmremote-' + str(size) + '.png' for size in SIZES]
            + ['favicon-' + str(size) + '.png' for size in FAVICON_SIZES])


def generate(source):
    if platform.system() != 'Darwin':
        raise SystemExit('Generation requires macOS sips/iconutil. Other platforms use the checked-in icons.')
    data = source.read_bytes()
    if digest(data) != SOURCE_SHA256 or png_size(data) != (1254, 1254):
        raise ValueError('Use the original supplied PNG without edits')
    ASSETS.mkdir(parents=True, exist_ok=True)
    master = ASSETS / 'source.png'
    if master.exists() and master.read_bytes() != data:
        raise ValueError('Refusing to replace a different branding master')
    master.write_bytes(data)
    workspace = ROOT / 'build/macos/branding'; workspace.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=workspace) as directory:
        temp = Path(directory)
        # App icons: transparent macOS grid (824 px squircle, shadow) so docks/taskbars show the shape, not a square.
        composed = temp / 'composed-1024.png'
        subprocess.run(['/usr/bin/swift', str(ROOT / 'scripts/branding-compose.swift'), str(master), str(composed),
                        *map(str, BODY), *map(str, BODY_RGB)], check=True)
        for size in SIZES:
            output = temp / ('cmremote-' + str(size) + '.png')
            subprocess.run(['/usr/bin/sips', '-z', str(size), str(size), str(composed), '--out', str(output)],
                           check=True, stdout=subprocess.DEVNULL)
        # The favicon keeps the supplied artwork's full framing.
        for size in FAVICON_SIZES:
            subprocess.run(['/usr/bin/sips', '-z', str(size), str(size), str(master), '--out', str(temp / ('favicon-' + str(size) + '.png'))],
                           check=True, stdout=subprocess.DEVNULL)
        composed.unlink()
        for name, prefix, sizes in [('cmremote.ico', 'cmremote-', ICO_SIZES), ('favicon.ico', 'favicon-', FAVICON_SIZES)]:
            (temp / name).write_bytes(ico_bytes([(size, (temp / (prefix + str(size) + '.png')).read_bytes()) for size in sizes]))
        iconset = temp / 'cmremote.iconset'; iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                suffix = '@2x' if scale == 2 else ''
                shutil.copyfile(temp / ('cmremote-' + str(size * scale) + '.png'), iconset / ('icon_' + str(size) + 'x' + str(size) + suffix + '.png'))
        subprocess.run(['/usr/bin/iconutil', '-c', 'icns', str(iconset), '-o', str(temp / 'cmremote.icns')], check=True)
        for path in sorted(temp.iterdir()):
            if path.is_file():
                shutil.copyfile(path, ASSETS / path.name)
    names = asset_names()
    manifest = dict(schema=1, source_name='cmremote Wireless Monogram Icon.png', source_sha256=SOURCE_SHA256,
                    composition='App icons: supplied body recomposed on the macOS icon grid (transparent 1024 canvas, '
                                '824 px superellipse, drop shadow). Favicon: original square image, resize only.',
                    generator='python3 scripts/branding.py generate (macOS swift CoreGraphics, sips, iconutil)',
                    files={name: digest((ASSETS / name).read_bytes()) for name in sorted(names)})
    (ASSETS / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    verify()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['generate', 'verify'])
    parser.add_argument('--source', type=Path, default=ASSETS / 'source.png', help='Original supplied PNG (generate only)')
    args = parser.parse_args()
    if args.command == 'generate':
        generate(args.source)
    else:
        if args.source != ASSETS / 'source.png':
            parser.error('--source requires generate')
        verify()
    print('Branding assets verified: ' + str(ASSETS))


if __name__ == '__main__':
    main()
