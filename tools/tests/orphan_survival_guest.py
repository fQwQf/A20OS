#!/usr/bin/python3
"""A service survives its killed spawn helper unless it requested PDEATHSIG."""
import os
import signal
import socket
import subprocess
import sys
import time
HELPER = r'''
import ctypes,os,socket,sys
channel=socket.socket(fileno=int(sys.argv[1]))
signum=int(sys.argv[2])
pid=os.fork()
if pid:
    channel.close()
    os.waitpid(pid,0)
    os._exit(0)
libc=ctypes.CDLL(None,use_errno=True)
if libc.prctl(1,signum,0,0,0):os._exit(77)
channel.sendall((str(os.getpid())+'\n').encode())
try:
    if channel.recv(1)==b'G':channel.sendall(b'alive')
finally:
    channel.close()
    os._exit(0)
'''
for parent_signal in (signal.SIGKILL, signal.SIGTERM):
    for requested in (0,signal.SIGTERM):
        left,right=socket.socketpair();left.settimeout(5)
        helper=subprocess.Popen([sys.executable,'-c',HELPER,str(right.fileno()),str(requested)],pass_fds=(right.fileno(),))
        right.close();child=None
        try:
            data=b''
            while not data.endswith(b'\n'):
                part=left.recv(64)
                assert part,'helper child exited before ready'
                data+=part
            child=int(data)
            helper.send_signal(parent_signal);helper.wait(timeout=5)
            assert helper.returncode==-parent_signal,helper.returncode
            time.sleep(0.1)
            if requested:
                assert left.recv(16)==b'', 'explicit parent-death signal was not delivered'
            else:
                left.sendall(b'G')
                assert left.recv(16)==b'alive', 'unconfigured descendant was killed with its parent'
            print('ORPHAN_SURVIVAL: case',int(parent_signal),'pdeathsig',int(requested),'PASS',flush=True)
        finally:
            left.close()
            if helper.poll() is None:helper.kill();helper.wait()
            if child:
                try:os.kill(child,signal.SIGKILL)
                except ProcessLookupError:pass
print('ORPHAN_SURVIVAL: PASS service survival and explicit parent-death signal',flush=True)
