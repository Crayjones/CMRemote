#!/usr/bin/env python3
"""Opt-in production VNC acceptance with an owned, loopback-only RFB 3.8 fixture.

Requires the parent's prebuilt vnc_integration target and project SDK. No Docker,
external VNC server, credentials, or third-party Python packages are used. None
security is the fixture default and connects without a prompt, like mRemoteNG.
/mode?password=1 makes new connections require VncAuth with a fixed challenge;
the response is checked against the DES vector for "password" (no DES here).
Offscreen results prove transport/CPU/input only, never native QRhi composition.
"""
import argparse
from collections import deque
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
import uuid

sys.dont_write_bytecode = True
from env import ROOT, environment

CASES = ('savedVncRenderingThroughShell', 'temporaryVncInputThroughShell',
         'vncFullscreenGeometryThroughShell', 'vncDisconnectReconnectThroughShell',
         'vncResponsiveCloseThroughShell', 'vncZrleTileBurstThroughShell', 'vncPasswordPromptThroughShell')
# DES response for password "password" to challenge bytes 0..15 (core/tests/vnc_transport.rs).
PASSWORD_RESPONSE = bytes.fromhex('b866924125c8eebb9debc1db61c538e2')
BGRA = bytes((32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0))
COLORS = ((224, 48, 80), (32, 192, 112), (48, 96, 224), (224, 192, 48))
CHANGED = (32, 200, 216)


def exact(stream, size):
    result = bytearray()
    while len(result) < size:
        chunk = stream.recv(size - len(result))
        if not chunk:
            raise EOFError
        result.extend(chunk)
    return bytes(result)


def rectangle(x, y, width, height, encoding, payload=b''):
    return struct.pack('!HHHHi', x, y, width, height, encoding) + payload


def pixel(color):
    red, green, blue = color
    return bytes((blue, green, red, 255))


def raw_pattern(width, height, changed=False):
    colors = (CHANGED if changed else COLORS[0], *COLORS[1:])
    rows = []
    for left, right in ((colors[0], colors[1]), (colors[2], colors[3])):
        rows.append(pixel(left) * (width // 2) + pixel(right) * (width - width // 2))
    data = rows[0] * (height // 2) + rows[1] * (height - height // 2)
    return rectangle(0, 0, width, height, 0, data)


def zrle_pattern(client):
    # One full-HD rectangle emits 510 solid tiles. Preserve the zlib stream
    # across updates, like a real ZRLE server; no terminated-stream shortcut.
    colors = (CHANGED if client.changed else COLORS[0], *COLORS[1:])
    tiles = bytearray()
    for y in range(0, client.height, 64):
        for x in range(0, client.width, 64):
            color = colors[(2 if y >= client.height // 2 else 0) + (x >= client.width // 2)]
            tiles.extend(b'\x01' + pixel(color)[:3])
    data = client.compressor.compress(tiles) + client.compressor.flush(zlib.Z_SYNC_FLUSH)
    return rectangle(0, 0, client.width, client.height, 16, struct.pack('!I', len(data)) + data)


def cursor_rectangle(hidden=False):
    if hidden:
        return rectangle(0, 0, 0, 0, -239)
    # Asymmetric hotspot, transparent right half, unmistakable opaque magenta.
    return rectangle(3, 5, 16, 16, -239, pixel((240, 32, 208)) * 256 + b'\xff\x00' * 16)


class Client:
    def __init__(self, number, stream):
        self.number, self.stream = number, stream
        self.width, self.height = 640, 360
        self.connected = self.authenticated = self.initial = False
        self.encodings = []
        self.pixel_format = []
        self.commands = deque()
        self.keys, self.pointers, self.request_sizes = deque(maxlen=512), deque(maxlen=512), deque(maxlen=64)
        self.pending = False
        self.hold = False
        self.changed = False
        self.requests = self.empty_updates = self.raw_updates = self.copy_updates = 0
        self.cursor_updates = self.resize_updates = self.key_count = self.pointer_count = 0
        self.max_outstanding = 0
        self.compressor = zlib.compressobj()
        self.zrle_updates = 0
        self.password_requested = self.password_accepted = False
        self.zrle_sent_at = None
        self.zrle_latencies_ms = deque(maxlen=64)

    def snapshot(self):
        return {key: getattr(self, key) for key in
                ('number', 'width', 'height', 'connected', 'authenticated', 'requests',
                 'empty_updates', 'raw_updates', 'copy_updates', 'cursor_updates',
                 'resize_updates', 'key_count', 'pointer_count', 'max_outstanding', 'hold', 'zrle_updates',
                 'password_requested', 'password_accepted')} | {
                    'zrle_latencies_ms': list(self.zrle_latencies_ms),
                    'keys': list(self.keys), 'pointers': list(self.pointers),
                    'request_sizes': list(self.request_sizes), 'encodings': self.encodings,
                    'pixel_format': self.pixel_format}


class OwnedRfb:
    """The oracle changes only this fixture; no command is executed on the host."""
    def __init__(self):
        self.owner = uuid.uuid4().hex
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.clients, self.threads, self.errors = [], [], []
        self.require_password = False
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(8)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        fixture = self

        class Oracle(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass  # No request or arbitrary client text is logged.

            def do_GET(self):
                route = urlsplit(self.path)
                query = parse_qs(route.query)
                status = 200
                with fixture.lock:
                    if query.get('owner') != [fixture.owner]:
                        status, reply = 403, {'error': 'owned fixture identity required'}
                    elif route.path == '/state':
                        reply = fixture.snapshot()
                    elif route.path == '/mode' and query.get('password') in (['0'], ['1']):
                        fixture.require_password = query['password'] == ['1']
                        reply = {'ok': True, 'owner': fixture.owner}
                    elif route.path == '/control':
                        try:
                            number = int(query.get('client', ['0'])[0])
                            client = next(item for item in fixture.clients if item.number == number)
                            action = query.get('action', [''])[0]
                            if action == 'clear-input':
                                client.keys.clear(); client.pointers.clear()
                            elif action == 'hold':
                                client.hold = True
                            elif action == 'resume':
                                client.hold = False
                            elif action in ('raw', 'zrle', 'copy', 'cursor', 'hide-cursor', 'resize', 'disconnect'):
                                if not client.connected:
                                    raise ValueError
                                client.commands.append(action)
                            else:
                                raise ValueError
                            reply = {'ok': True, 'owner': fixture.owner}
                        except (ValueError, StopIteration):
                            status, reply = 400, {'error': 'invalid owned fixture control'}
                    else:
                        status, reply = 404, {'error': 'unknown owned fixture route'}
                data = json.dumps(reply, separators=(',', ':')).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.oracle = ThreadingHTTPServer(('127.0.0.1', 0), Oracle)
        self.oracle.daemon_threads = True
        self.oracle_port = self.oracle.server_address[1]
        self.acceptor = threading.Thread(target=self.accept, daemon=True)
        self.http_thread = threading.Thread(target=self.oracle.serve_forever, daemon=True)

    def snapshot(self):
        return {'schema': 1, 'owner': self.owner, 'host': '127.0.0.1',
                'accepted': len(self.clients), 'authenticated': sum(c.authenticated for c in self.clients),
                'active': sum(c.connected for c in self.clients),
                'errors': list(self.errors), 'clients': [c.snapshot() for c in self.clients]}

    def __enter__(self):
        self.acceptor.start(); self.http_thread.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.listener.close()
        with self.lock:
            for client in self.clients:
                try:
                    client.stream.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
        self.oracle.shutdown(); self.oracle.server_close()
        self.acceptor.join(2); self.http_thread.join(2)
        for thread in self.threads:
            thread.join(2)
        if any(thread.is_alive() for thread in [self.acceptor, self.http_thread, *self.threads]):
            raise RuntimeError('Owned VNC fixture did not retire')

    def accept(self):
        while not self.stop.is_set():
            try:
                stream, address = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            if address[0] != '127.0.0.1':
                stream.close(); continue
            stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            with self.lock:
                client = Client(len(self.clients) + 1, stream)
                self.clients.append(client)
                thread = threading.Thread(target=self.serve, args=(client,), daemon=True)
                self.threads.append(thread)
            thread.start()

    def serve(self, client):
        stream = client.stream
        try:
            # Only negotiation has a deadline. Connected idle/read silence is valid.
            stream.settimeout(8)
            stream.sendall(b'RFB 003.008\n')
            if exact(stream, 12) != b'RFB 003.008\n':
                raise ValueError('unexpected RFB version')
            with self.lock:
                password = self.require_password
            if password:
                stream.sendall(b'\x01\x02')  # VncAuth only.
                if exact(stream, 1) != b'\x02':
                    raise ValueError('unexpected fixture security selection')
                with self.lock:
                    client.password_requested = True
                # The user types the password while this waits; no deadline here.
                stream.settimeout(None)
                stream.sendall(bytes(range(16)))
                if exact(stream, 16) != PASSWORD_RESPONSE:
                    stream.sendall(struct.pack('!II', 1, 6) + b'denied')
                    raise ValueError('fixture password rejected')
                stream.settimeout(8)
                with self.lock:
                    client.password_accepted = True
            else:
                stream.sendall(b'\x01\x01')  # None only: the client connects without a prompt.
                if exact(stream, 1) != b'\x01':
                    raise ValueError('unexpected fixture security selection')
            stream.sendall(struct.pack('!I', 0))
            if exact(stream, 1) != b'\x01':
                raise ValueError('fixture requires shared ClientInit')
            name = b'Owned loopback RFB acceptance'
            stream.sendall(struct.pack('!HH', client.width, client.height) + BGRA + struct.pack('!I', len(name)) + name)
            with self.lock:
                client.authenticated = client.connected = True
            stream.settimeout(None)  # No blanket connected-session idle timeout.
            while not self.stop.is_set():
                with self.lock:
                    if client.commands and client.commands[0] == 'disconnect':
                        client.commands.popleft()
                        return
                    if client.pending and not client.hold:
                        message = self.update(client)
                        client.pending = False
                    else:
                        message = None
                if message is not None:
                    stream.sendall(message)
                if not select.select([stream], [], [], 0.05)[0]:
                    continue
                kind = exact(stream, 1)[0]
                if kind == 0:
                    data = exact(stream, 19)
                    if data[:3] != b'\0' * 3 or data[3:] != BGRA:
                        raise ValueError('unexpected fixture pixel format')
                    with self.lock:
                        client.pixel_format = list(data[3:])
                elif kind == 2:
                    padding, count = struct.unpack('!BH', exact(stream, 3))
                    if padding or count > 32:
                        raise ValueError('invalid fixture encoding count')
                    encodings = list(struct.unpack('!' + 'i' * count, exact(stream, count * 4)))
                    if not all(value in encodings for value in (0, 1, -239, -223)):
                        raise ValueError('required fixture encodings not negotiated')
                    with self.lock:
                        client.encodings = encodings
                elif kind == 3:
                    incremental, x, y, width, height = struct.unpack('!BHHHH', exact(stream, 9))
                    with self.lock:
                        if client.pending:
                            raise ValueError('pipelined framebuffer requests')
                        if not client.encodings or not client.pixel_format:
                            raise ValueError('framebuffer requested before negotiation')
                        if incremental not in (0, 1) or not width or not height or x + width > client.width or y + height > client.height:
                            raise ValueError('invalid framebuffer request bounds')
                        if client.zrle_sent_at is not None:
                            client.zrle_latencies_ms.append((time.monotonic() - client.zrle_sent_at) * 1000)
                            client.zrle_sent_at = None
                        client.requests += 1
                        client.request_sizes.append({'incremental': incremental, 'x': x, 'y': y, 'width': width, 'height': height})
                        client.pending = True
                        client.max_outstanding = max(client.max_outstanding, 1)
                elif kind == 4:
                    down, padding, symbol = struct.unpack('!BHI', exact(stream, 7))
                    if down not in (0, 1) or padding:
                        raise ValueError('invalid fixture key message')
                    with self.lock:
                        client.key_count += 1
                        client.keys.append({'sym': symbol, 'down': bool(down)})
                elif kind == 5:
                    mask, x, y = struct.unpack('!BHH', exact(stream, 5))
                    with self.lock:
                        if x >= client.width or y >= client.height:
                            raise ValueError('pointer outside current server geometry')
                        client.pointer_count += 1
                        client.pointers.append({'mask': mask, 'x': x, 'y': y})
                elif kind == 6:
                    padding, length = exact(stream, 3), struct.unpack('!I', exact(stream, 4))[0]
                    if padding != b'\0' * 3 or length > 4096:
                        raise ValueError('unbounded fixture clipboard message')
                    exact(stream, length)
                    raise ValueError('VNC unexpectedly sent clipboard data')
                else:
                    raise ValueError('unexpected VNC client message type')
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass  # Expected disconnect/cancellation paths.
        except (ValueError, OSError) as error:
            if not self.stop.is_set():
                with self.lock:
                    # Fixed diagnostic strings only; no wire payload/credentials.
                    self.errors.append(str(error) if isinstance(error, ValueError) else 'fixture socket failure')
        finally:
            with self.lock:
                client.connected = False
            stream.close()

    def update(self, client):
        rectangles = []
        if not client.initial:
            rectangles = [raw_pattern(client.width, client.height), cursor_rectangle()]
            client.initial = True
            client.raw_updates += 1; client.cursor_updates += 1
        elif client.commands:
            action = client.commands.popleft()
            if action == 'raw':
                client.changed = not client.changed
                rectangles = [raw_pattern(client.width, client.height, client.changed)]
                client.raw_updates += 1
            elif action == 'zrle':
                if 16 not in client.encodings:
                    raise ValueError('ZRLE not negotiated')
                if (client.width, client.height) != (1920, 1080):
                    client.width, client.height = 1920, 1080
                    # RFC 6143 requires DesktopSize to be the last rectangle.
                    # Send it alone, then the tile burst on the next request.
                    rectangles.append(rectangle(0, 0, client.width, client.height, -223))
                    client.resize_updates += 1
                    client.commands.appendleft(action)
                else:
                    client.changed = not client.changed
                    rectangles.append(zrle_pattern(client))
                    client.zrle_updates += 1
                    client.zrle_sent_at = time.monotonic()
            elif action == 'copy':
                rectangles = [rectangle(client.width - 160, client.height - 112, 80, 64, 1, struct.pack('!HH', 16, 16))]
                client.copy_updates += 1
            elif action in ('cursor', 'hide-cursor'):
                rectangles = [cursor_rectangle(action == 'hide-cursor')]
                client.cursor_updates += 1
            elif action == 'resize':
                client.width, client.height = 800, 480
                client.changed = False
                rectangles = [rectangle(0, 0, client.width, client.height, -223),
                              raw_pattern(client.width, client.height), cursor_rectangle()]
                client.resize_updates += 1; client.raw_updates += 1; client.cursor_updates += 1
            else:
                raise ValueError('invalid queued fixture action')
        else:
            # Valid, completed empty incremental replies keep a quiet desktop alive.
            client.empty_updates += 1
        return struct.pack('!BBH', 0, 0, len(rectangles)) + b''.join(rectangles)


def load_runner(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def accepted_report(report):
    totals = re.search(r'^Totals: (\d+) passed, (\d+) failed, (\d+) skipped,', report, re.M)
    return bool(totals and int(totals[1]) >= 3 and int(totals[2]) == int(totals[3]) == 0
                and re.search(r'^\*+ Finished testing of VncIntegration \*+$', report, re.M)
                and not any(value in report.lower() for value in ('qfatal', 'unreleased resources')))


def pending(reason):
    print('PENDING: ' + reason + '; no native VNC acceptance evidence claimed.', file=sys.stderr)
    return 77


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('offscreen', 'cocoa', 'xcb'), default='offscreen')
    parser.add_argument('cases', nargs='*', help='QtTest function names; default runs all, one owned fixture per function')
    args = parser.parse_args()
    cases = args.cases or CASES
    if any(case not in CASES for case in cases):
        parser.error('Select a VNC integration test function')
    if args.platform == 'cocoa' and sys.platform != 'darwin':
        return pending('Cocoa requires a macOS SDK and native desktop')
    if args.platform == 'xcb' and (sys.platform != 'linux' or not os.environ.get('DISPLAY')):
        return pending('xcb currently requires a local Linux SDK and existing native DISPLAY (container wrapper not implemented)')
    env = environment()
    build = Path(env['RM_BUILD_DIR']).resolve()
    binary = build / 'app/vnc_integration'
    if not binary.is_file():
        return pending('Parent must build vnc_integration before this opt-in runner is used')
    base = build / 'test-data'
    base.mkdir(parents=True, exist_ok=True)
    outcomes = []
    for case in cases:
        # Evidence remains in the build tree. Runtime manifests/XML/config are scoped
        # to this run, never user preferences or the operating system temp directory.
        run = Path(tempfile.mkdtemp(prefix='vnc-' + case + '-', dir=base))
        run.chmod(0o700)
        config, temporary, evidence = (run / name for name in ('config', 'tmp', 'evidence'))
        for directory in (config, temporary, evidence):
            directory.mkdir(mode=0o700)
        with OwnedRfb() as fixture:
            manifest = {'schema': 1, 'opt_in': True, 'owner': fixture.owner, 'host': '127.0.0.1',
                        'rfb_port': fixture.port, 'oracle_port': fixture.oracle_port,
                        'platform': args.platform, 'evidence_dir': str(evidence)}
            (config / 'vnc-fixture.json').write_text(json.dumps(manifest))
            run_env = dict(env, QT_QPA_PLATFORM=args.platform, QT_RHI_LEAK_CHECK='1',
                           XDG_CONFIG_HOME=str(config), XDG_DATA_HOME=str(config),
                           XDG_CACHE_HOME=str(config), TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary))
            # None fixture never needs credentials; do not pass unrelated test secrets.
            for key in tuple(run_env):
                if key.startswith(('RM_SSH_', 'RM_RDP_', 'RM_XML_TEST_')):
                    run_env.pop(key)
            try:
                if args.platform == 'cocoa':
                    result = load_runner('test-cocoa').run_cocoa('vnc_integration', [case], run_env, save_report=False)
                else:
                    result = subprocess.run([str(binary), case], cwd=ROOT, env=run_env,
                                            capture_output=True, text=True, timeout=150)
                report = result.stdout + result.stderr
                print(report, end='' if report.endswith('\n') else '\n')
                (run / 'results.txt').write_text(report)
                with fixture.lock:
                    observed = fixture.snapshot()
                (run / 'oracle.json').write_text(json.dumps(observed, indent=2))
                passed = result.returncode == 0 and accepted_report(report) and not observed['errors']
                native = args.platform != 'offscreen'
                marker = 'RM_VNC_NATIVE_COMPOSITION=1' if native else 'RM_VNC_NATIVE_COMPOSITION=0'
                passed = passed and marker in report
                outcomes.append({'case': case, 'passed': passed, 'native_composition': native and passed,
                                 'report': str(run / 'results.txt'), 'evidence': str(evidence)})
                print('Owned VNC report: ' + str(run / 'results.txt'))
                if not passed:
                    print('VNC acceptance failed; no native evidence claimed for this case.', file=sys.stderr)
                    return 1
            except subprocess.TimeoutExpired:
                (run / 'results.txt').write_text('FAILED: owned VNC acceptance exceeded 150s.\n')
                print('Owned VNC acceptance timed out; report: ' + str(run / 'results.txt'), file=sys.stderr)
                return 1
            finally:
                (config / 'vnc-fixture.json').unlink(missing_ok=True)
    print(json.dumps({'platform': args.platform, 'native_composition': args.platform != 'offscreen',
                      'scope': 'owned RFB 3.8 None fixture only; not encrypted/authenticated interoperability',
                      'results': outcomes}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
