#!/usr/bin/env python3
"""Exec/fork and actual user-page semantics of /proc/<pid>/environ."""
import os
import subprocess
import sys


def main():
    first = subprocess.check_output(['md5sum', '/proc/self/environ'], env=dict(os.environ, A20_PROC_ENV='a'))
    second = subprocess.check_output(['md5sum', '/proc/self/environ'], env=dict(os.environ, A20_PROC_ENV='b'))
    assert first != second, 'OpenRC mount detection needs per-exec environments'
    code = r'''
import ctypes,os
raw=open('/proc/self/environ','rb').read()
assert b'A20_PROC_ENV_LONG='+b'x'*16384+b'\0' in raw
assert os.stat('/proc/self/environ').st_size==0
libc=ctypes.CDLL(None);libc.getenv.restype=ctypes.c_void_p
address=libc.getenv(b'A20_PROC_ENV_MUTABLE');assert address
value=(ctypes.c_char*4).from_address(address);value[0]=b'B'
assert b'A20_PROC_ENV_MUTABLE=BAAA\0' in open('/proc/self/environ','rb').read()
parent=os.getpid();child=os.fork()
if child==0:
 assert b'A20_PROC_ENV_MUTABLE=BAAA\0' in open('/proc/self/environ','rb').read()
 assert b'A20_PROC_ENV_MUTABLE=BAAA\0' in open('/proc/%d/environ'%parent,'rb').read()
 os._exit(0)
_,status=os.waitpid(child,0);assert status==0,status
'''
    subprocess.run([sys.executable, '-c', code], check=True,
                   env=dict(os.environ, A20_PROC_ENV_LONG='x'*16384, A20_PROC_ENV_MUTABLE='AAAA'))
    print('PROC_ENVIRON: PASS exec isolation, 16 KiB, live user pages, fork and parent', flush=True)


if __name__ == '__main__':
    main()
