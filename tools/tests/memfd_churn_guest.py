#!/usr/bin/python3
"""Dirty shared-buffer churn exceeds the kernel's 1 GiB cache cap three times."""
import array
import mmap
import os
from pathlib import Path
import socket
import time

def memory():
    return {k:int(v.split()[0]) for k,v in
            (line.split(':',1) for line in Path('/proc/meminfo').read_text().splitlines())}

# The receiver and mappings must survive closing the sender's fd and its dup.
left,right=socket.socketpair()
fd=os.memfd_create('rights-lifetime',os.MFD_CLOEXEC)
os.ftruncate(fd,4096)
alias=os.dup(fd)
left.sendmsg([b'x'],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[alias]))])
os.close(fd);os.close(alias)
_,rights,_,_=right.recvmsg(1,socket.CMSG_SPACE(array.array('i').itemsize))
received=array.array('i');received.frombytes(rights[0][2]);assert len(received)==1
fd=received[0]
a=mmap.mmap(fd,4096,flags=mmap.MAP_SHARED,prot=mmap.PROT_READ|mmap.PROT_WRITE)
b=mmap.mmap(fd,4096,flags=mmap.MAP_SHARED,prot=mmap.PROT_READ|mmap.PROT_WRITE)
os.close(fd);a[0:4]=b'live';assert b[:4]==b'live';a.close();assert b[:4]==b'live';b.close()
left.close();right.close()
print('MEMFD_CHURN: PASS dup and SCM_RIGHTS mappings survive descriptor close',flush=True)

before=memory();size=16*1024*1024;started=time.monotonic()
for i in range(192):
    fd=os.memfd_create('wayland-churn',os.MFD_CLOEXEC)
    os.ftruncate(fd,size)
    a=mmap.mmap(fd,size,flags=mmap.MAP_SHARED,prot=mmap.PROT_READ|mmap.PROT_WRITE)
    b=mmap.mmap(fd,size,flags=mmap.MAP_SHARED,prot=mmap.PROT_READ|mmap.PROT_WRITE)
    os.close(fd)
    pattern=bytes([1+i%251])
    a[::4096]=pattern*(size//4096)
    assert b[::4096]==pattern*(size//4096)
    a.close();assert b[0:1]==pattern
    b.close()
    if (i+1)%32==0:
        now=memory()
        print('MEMFD_CHURN:',(i+1)*16,'MiB dirty-buffer churn',
              'cache_kib',now['Cached'],'dirty_kib',now['Dirty'],flush=True)
        assert now['Dirty']-before['Dirty']<128*1024,('dirty memfd pages leaked',before,now)
        assert before['MemFree']-now['MemFree']<256*1024,('buffer frames leaked',before,now)
print('MEMFD_CHURN: PASS 3072 MiB; mappings intact and memory bounded; seconds',
      round(time.monotonic()-started,2),flush=True)
