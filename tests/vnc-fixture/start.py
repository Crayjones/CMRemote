#!/usr/bin/env python3
"""Owned TigerVNC fixture: one Xvnc display (with an XFCE desktop) per VeNCrypt subtype. No credentials appear in commands."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, '/fixture')
from ports import PORTS, USER


def main():
    password = os.environ.pop('RM_VNC_TEST_PASSWORD')
    # VncAuth keys DES with the first eight bytes; longer passwords would silently differ per type.
    if not 6 <= len(password) <= 8 or not password.isascii() or not password.isalnum():
        raise ValueError('Invalid synthetic password')
    subprocess.run(['chpasswd'], input=USER + ':' + password + '\n', text=True, check=True)
    tls = Path('/fixture/tls'); tls.mkdir(mode=0o700, exist_ok=True)
    vncpasswd = subprocess.run(['vncpasswd', '-f'], input=(password + '\n').encode(),
                               capture_output=True, check=True).stdout
    del password
    (tls / 'vncpasswd').write_bytes(vncpasswd); (tls / 'vncpasswd').chmod(0o600)
    # A private CA so clients can pin ca.pem for X509* types instead of trusting the leaf.
    openssl = lambda *args: subprocess.run(['openssl', *args], check=True, capture_output=True)
    openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '3650', '-subj', '/CN=RemoteManager VNC fixture CA',
            '-keyout', str(tls / 'ca.key'), '-out', str(tls / 'ca.pem'))
    openssl('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
            '-keyout', str(tls / 'server.key'), '-out', str(tls / 'server.csr'))
    (tls / 'san.cnf').write_text('subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n')
    openssl('x509', '-req', '-in', str(tls / 'server.csr'), '-CA', str(tls / 'ca.pem'), '-CAkey', str(tls / 'ca.key'),
            '-CAcreateserial', '-days', '3650', '-extfile', str(tls / 'san.cnf'), '-out', str(tls / 'server.pem'))

    # Per-display XDG directories keep the seven XFCE sessions of one user apart. Each starts
    # with xfwm4 dragging windows to a screen edge to tile them (left/right halves, corners,
    # top maximizes) instead of wrapping to the next workspace, and the bottom dock always
    # shown instead of sliding in and out as windows pass over it.
    panel = Path('/etc/xdg/xfce4/panel/default.xml').read_text()
    panel = panel.replace('<property name="autohide-behavior" type="uint" value="1"/>', '<property name="autohide-behavior" type="uint" value="0"/>')
    xfwm4 = ('<?xml version="1.0" encoding="UTF-8"?>\n<channel name="xfwm4" version="1.0">\n  <property name="general" type="empty">\n'
             '    <property name="wrap_windows" type="bool" value="false"/>\n    <property name="tile_on_move" type="bool" value="true"/>\n'
             '    <property name="snap_to_border" type="bool" value="true"/>\n  </property>\n</channel>\n')
    for display in range(1, len(PORTS) + 1):
        runtime = Path('/tmp/runtime-%s-%d' % (USER, display)); runtime.mkdir(mode=0o700, exist_ok=True)
        shutil.chown(runtime, USER, USER)
        config = Path('/home/%s/.config-%d' % (USER, display))
        channels = config / 'xfce4/xfconf/xfce-perchannel-xml'; channels.mkdir(parents=True, exist_ok=True)
        (channels / 'xfwm4.xml').write_text(xfwm4); (channels / 'xfce4-panel.xml').write_text(panel)
        for path in [config, *config.rglob('*')]: shutil.chown(path, USER, USER)
    servers = []
    for display, (port, types, _, _) in enumerate(PORTS, start=1):
        log = Path('/fixture/xvnc-%d.log' % port).open('wb')
        servers.append(subprocess.Popen(['Xvnc', ':%d' % display, '-rfbport', str(port), '-SecurityTypes', types,
            '-PlainUsers', USER, '-PAMService', 'tigervnc', '-rfbauth', str(tls / 'vncpasswd'),
            '-X509Cert', str(tls / 'server.pem'), '-X509Key', str(tls / 'server.key'),
            '-geometry', '1280x800', '-depth', '24', '-AlwaysShared', '-desktop', types + ' :' + str(port),
            # Only an explicit copy (CLIPBOARD) reaches the client, not every text selection (PRIMARY).
            '-SendPrimary=0'],
            stdout=log, stderr=subprocess.STDOUT))
        for _ in range(100):
            if Path('/tmp/.X11-unix/X%d' % display).exists(): break
            time.sleep(0.05)
        # A full XFCE desktop per display, each with its own session bus, plus a terminal that
        # names the port so a wrong connection is obvious.
        env = {'DISPLAY': ':%d' % display, 'HOME': '/home/' + USER, 'USER': USER, 'PATH': os.environ['PATH'],
               'XDG_CONFIG_HOME': '/home/%s/.config-%d' % (USER, display), 'XDG_CACHE_HOME': '/home/%s/.cache-%d' % (USER, display),
               'XDG_RUNTIME_DIR': '/tmp/runtime-%s-%d' % (USER, display), 'LANG': 'C.UTF-8'}
        label = 'echo "Security: %s  (container port %d)"; exec bash' % (types, port)
        session = ('(sleep 3; xfce4-terminal --geometry 90x24+60+60 --title "%s" -x sh -c \'%s\') &'
                   ' exec startxfce4') % (types, label)
        subprocess.Popen(['runuser', '-u', USER, '--', 'dbus-launch', '--exit-with-session', 'sh', '-c', session],
                         env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Any server exiting fails the fixture rather than leaving a silently missing port;
    # a desktop that exits is only reaped.
    pids = {server.pid for server in servers}
    while os.wait()[0] not in pids: pass
    raise SystemExit('Xvnc exited; see /fixture/xvnc-*.log')


if __name__ == '__main__': main()
