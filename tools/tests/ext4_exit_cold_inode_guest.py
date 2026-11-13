#!/usr/bin/python3
"""Snapshot-only: terminate owners of unlinked files after inode-cache eviction."""
import ctypes,mmap,os,signal,subprocess,tempfile,time,traceback

def main():
 with tempfile.TemporaryDirectory(prefix='a20-cold-exit-') as base:
  r,w=os.pipe();children=[]
  for index in range(4):
   pid=os.fork()
   if pid==0:
    os.close(r)
    try:
     for number in range(32):
      path=base+'/'+str(index)+'-'+str(number)
      fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
      assert os.write(fd,b'X'*4096)==4096
      os.unlink(path);assert os.fstat(fd).st_nlink==0
      # Keep each descriptor alive until the default SIGTERM teardown.
     assert os.write(w,b'R')==1
     while True:signal.pause()
    except BaseException:
     traceback.print_exc();os._exit(1)
   children.append(pid)
  os.close(w);ready=b''
  while len(ready)<4:
   chunk=os.read(r,4-len(ready));assert chunk;ready+=chunk
  os.close(r);os.sync()
  print('COLD_EXIT: unlinked descriptors held by 4 child processes',flush=True)
  # O_DIRECT bypasses the file page cache. One aligned 4 KiB read still
  # traverses ext4's block cache, pushing out its clean inode-table pages.
  fd=os.open('/usr/lib/firefox/libxul.so',os.O_RDONLY|os.O_DIRECT)
  buf=mmap.mmap(-1,4096);address=ctypes.addressof(ctypes.c_char.from_buffer(buf))
  libc=ctypes.CDLL(None,use_errno=True);libc.pread.argtypes=[ctypes.c_int,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_longlong];libc.pread.restype=ctypes.c_ssize_t
  size=min(os.fstat(fd).st_size//4096*4096,96<<20);assert size>72<<20
  start=time.monotonic()
  try:
   for offset in range(0,size,4096):
    count=libc.pread(fd,address,4096,offset);assert count==4096,(offset,count,ctypes.get_errno())
  finally:os.close(fd);buf.close()
  print('COLD_EXIT: pressure_bytes='+str(size)+' seconds='+str(time.monotonic()-start),flush=True)
  for pid in children:os.kill(pid,signal.SIGTERM)
  for pid in children:
   _,status=os.waitpid(pid,0);assert os.WIFSIGNALED(status) and os.WTERMSIG(status)==signal.SIGTERM,status
  ring=subprocess.check_output(['dmesg','-s','1048576'],text=True)
  bad=('poison caller=','transaction poisoned','deferred orphan cleanup failed','inode read failed','page fill failed')
  assert not any(marker in ring for marker in bad),'inode cleanup/read failure'
 print('COLD_EXIT: PASS cold inode reads during SIGTERM teardown',flush=True)
try:main()
except BaseException:
 print('A20_TEST_FATAL: cold inode exit failed',flush=True)
 raise
