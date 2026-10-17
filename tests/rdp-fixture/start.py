import os
from pathlib import Path
import subprocess

def replace_once(path, before, after):
    file = Path(path)
    text = file.read_text()
    if text.count(before) != 1: raise RuntimeError('Unexpected owned fixture configuration')
    file.write_text(text.replace(before, after, 1))


# A second container has its own PAM credentials, filesystem and event log.
# Only its network namespace is shared; Xorg abstract socket IDs must differ.
if os.environ.pop('RM_RDP_TEST_SECONDARY', '') == '1':
    replace_once('/etc/xrdp/xrdp.ini', '\nport=3389\n', '\nport=3390\n')
    replace_once('/etc/xrdp/sesman.ini', '\nX11DisplayOffset=10\n', '\nX11DisplayOffset=20\n')

password = os.environ.pop('RM_RDP_TEST_PASSWORD')
subprocess.run(['chpasswd'], input='rdpfixture:' + password + '\n', text=True, check=True)
del password
Path('/run/xrdp').mkdir(exist_ok=True)
subprocess.run(['chown', 'xrdp:xrdp', '/run/xrdp'], check=True)
subprocess.Popen(['/usr/sbin/xrdp-sesman', '--nodaemon'])
os.execv('/usr/sbin/xrdp', ['xrdp', '--nodaemon'])
