#!/usr/bin/env python3
"""Independent RFB 3.8 handshake per fixture port, through ServerInit. Prints one JSON line per port."""
import json
import os
import socket
import ssl
import struct
import subprocess
import sys

sys.path.insert(0, '/fixture')
from ports import PORTS, USER


def exact(stream, size):
    data = b''
    while len(data) < size:
        chunk = stream.recv(size - len(data))
        if not chunk: raise EOFError('server closed during handshake')
        data += chunk
    return data


def vnc_response(password, challenge):
    # RFB's DES key is the password with each byte's bits reversed.
    key = bytes(int('{:08b}'.format(byte)[::-1], 2) for byte in password.encode().ljust(8, b'\0')[:8])
    return subprocess.run(['openssl', 'enc', '-des-ecb', '-nopad', '-K', key.hex(), '-provider', 'legacy', '-provider', 'default'],
                          input=challenge, capture_output=True, check=True).stdout


def handshake(port, security, subtype, password):
    stream = socket.create_connection(('127.0.0.1', port), timeout=5)
    if exact(stream, 12) != b'RFB 003.008\n': raise ValueError('unexpected protocol version')
    stream.sendall(b'RFB 003.008\n')
    offered = list(exact(stream, exact(stream, 1)[0]))
    if security not in offered: raise ValueError('type %d not offered: %s' % (security, offered))
    stream.sendall(bytes((security,)))
    result = {'port': port, 'offered': offered}
    if security == 19:
        if exact(stream, 2) != b'\0\2': raise ValueError('unexpected VeNCrypt version')
        stream.sendall(b'\0\2')
        if exact(stream, 1) != b'\0': raise ValueError('VeNCrypt version refused')
        count = exact(stream, 1)[0]
        result['subtypes'] = list(struct.unpack('!%dI' % count, exact(stream, 4 * count)))
        if subtype not in result['subtypes']: raise ValueError('subtype %d not offered' % subtype)
        stream.sendall(struct.pack('!I', subtype))
        # Only TLS subtypes get a one-byte go-ahead before the TLS handshake; Plain has none.
        if subtype != 256 and exact(stream, 1) != b'\1': raise ValueError('TLS setup refused')
        if subtype in (258, 259):
            # Anonymous TLS has no TLS 1.3 suite; TigerVNC negotiates ADH/AECDH on 1.2.
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.check_hostname, context.verify_mode = False, ssl.CERT_NONE
            context.maximum_version = ssl.TLSVersion.TLSv1_2
            context.set_ciphers('aNULL:@SECLEVEL=0')
            stream = context.wrap_socket(stream)
        elif subtype in (261, 262):
            context = ssl.create_default_context(cafile='/fixture/tls/ca.pem')
            stream = context.wrap_socket(stream, server_hostname='localhost')
        if stream.__class__ is ssl.SSLSocket:
            result['tls'] = '%s %s' % (stream.version(), stream.cipher()[0])
        if subtype in (256, 259, 262):
            user, secret = USER.encode(), password.encode()
            stream.sendall(struct.pack('!II', len(user), len(secret)) + user + secret)
        else:
            stream.sendall(vnc_response(password, exact(stream, 16)))
    else:
        stream.sendall(vnc_response(password, exact(stream, 16)))
    status = struct.unpack('!I', exact(stream, 4))[0]
    if status:
        raise ValueError('authentication failed: ' + exact(stream, struct.unpack('!I', exact(stream, 4))[0]).decode(errors='replace'))
    stream.sendall(b'\1')  # ClientInit, shared
    width, height = struct.unpack('!HH', exact(stream, 4)); exact(stream, 16)
    result['name'] = exact(stream, struct.unpack('!I', exact(stream, 4))[0]).decode()
    result['size'] = [width, height]
    stream.close()
    return result


def main():
    password = os.environ.pop('RM_VNC_TEST_PASSWORD')
    failed = False
    for port, _, security, subtype in PORTS:
        try:
            print(json.dumps({'ok': True} | handshake(port, security, subtype, password)))
        except Exception as error:
            failed = True
            print(json.dumps({'ok': False, 'port': port, 'error': '%s: %s' % (type(error).__name__, error)}))
    raise SystemExit(failed)


if __name__ == '__main__': main()
