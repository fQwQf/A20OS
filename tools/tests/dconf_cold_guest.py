#!/usr/bin/python3
"""Exercise real dconf auto-activation in isolated profiles and private buses."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
INNER = r'''
import subprocess
schema='org.freedesktop.ibus.general';key='switcher-delay-time'
for value in ('123','456'):
    changed=subprocess.run(['gsettings','set',schema,key,value],capture_output=True,text=True,timeout=15)
    read=subprocess.run(['gsettings','get',schema,key],capture_output=True,text=True,timeout=15)
    print('DCONF_COLD:',value,'set',changed.returncode,'read',read.returncode,repr(read.stdout),repr(changed.stderr+read.stderr),flush=True)
    assert changed.returncode==0 and read.returncode==0 and read.stdout.strip()==value
'''
for attempt in range(5):
    with tempfile.TemporaryDirectory(prefix='a20-dconf-cold-') as config:
        env=dict(os.environ,XDG_CONFIG_HOME=config)
        env.pop('DBUS_STARTER_ADDRESS',None);env.pop('DBUS_STARTER_BUS_TYPE',None)
        result=subprocess.run(['dbus-run-session','--',sys.executable,'-c',INNER],env=env,capture_output=True,text=True,timeout=60)
        print('DCONF_COLD: attempt',attempt,result.returncode,result.stdout,result.stderr,flush=True)
        assert result.returncode==0, 'cold dconf activation or cross-process preference readback failed'
print('DCONF_COLD: PASS five fresh buses and isolated profiles',flush=True)
