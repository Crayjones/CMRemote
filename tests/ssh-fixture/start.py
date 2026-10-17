#!/usr/bin/env python3
"""Owned independent OpenSSH fixture. No credentials appear in commands or reports."""
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import pwd
import subprocess
import threading


def main():
    password = os.environ.pop('RM_SSH_TEST_PASSWORD')
    if not password or '\n' in password or ':' in password: raise ValueError('Invalid synthetic password')
    subprocess.run(['chpasswd'], input='sshfixture:' + password + '\n', text=True, check=True)
    key = os.environ.pop('RM_SSH_AUTHORIZED_KEY')
    if not key.startswith('ssh-ed25519 ') or '\n' in key: raise ValueError('Invalid synthetic public key')
    account = pwd.getpwnam('sshfixture')
    home = Path(account.pw_dir); ssh = home / '.ssh'; ssh.mkdir(mode=0o700)
    authorized = ssh / 'authorized_keys'; authorized.write_text(key + '\n'); authorized.chmod(0o600)
    for path in (ssh, authorized): os.chown(path, account.pw_uid, account.pw_gid)
    (home / '.bashrc').write_text("export LANG=C.UTF-8 LC_ALL=C.UTF-8\nexport PS1='owned-ssh> '\nstty -echoctl\n")
    for name in ('host', 'previous'):
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', '/fixture/' + name], check=True)
    Path('/fixture/sshd_config').write_text('''Port 22
ListenAddress 0.0.0.0
HostKey /fixture/host
PidFile /run/sshd-fixture.pid
PasswordAuthentication yes
KbdInteractiveAuthentication no
PubkeyAuthentication yes
PermitRootLogin no
AllowUsers sshfixture
UsePAM no
PrintMotd no
X11Forwarding no
AllowTcpForwarding no
AllowAgentForwarding no
PermitTunnel no
LogLevel VERBOSE
''')
    log = Path('/fixture/sshd.log')
    server = subprocess.Popen(['/usr/sbin/sshd', '-D', '-e', '-f', '/fixture/sshd_config'], stderr=log.open('wb'))

    class Oracle(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_GET(self):
            if self.path == '/state':
                lines = log.read_text(errors='replace').splitlines()
                response = {'auth': sum('Accepted password for sshfixture' in line or 'Accepted publickey for sshfixture' in line for line in lines)}
            elif self.path in ('/marker', '/vim', '/stty', '/first', '/second', '/shortcuts'):
                path = home / self.path[1:]
                response = {'exists': path.is_file(), 'text': path.read_text(errors='replace')[:4096] if path.is_file() else ''}
            else:
                self.send_error(404); return
            data = json.dumps(response).encode(); self.send_response(200)
            self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)

    oracle = ThreadingHTTPServer(('0.0.0.0', 8022), Oracle)
    threading.Thread(target=oracle.serve_forever, daemon=True).start()
    raise SystemExit(server.wait())


if __name__ == '__main__': main()
