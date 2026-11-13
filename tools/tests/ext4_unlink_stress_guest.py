#!/usr/bin/python3
"""Snapshot-only concurrent open/unlink, truncate and replacement semantics."""
import os,subprocess,tempfile,traceback

def worker(directory,loops):
 os.mkdir(directory)
 for i in range(loops):
  a=directory+'/a';b=directory+'/b';link=directory+'/link'
  fd=os.open(a,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
  try:
   assert os.write(fd,b'A'*4096)==4096
   os.unlink(a);assert os.fstat(fd).st_nlink==0
   os.lseek(fd,0,0);assert os.read(fd,4096)==b'A'*4096
   os.ftruncate(fd,2048);assert os.fstat(fd).st_size==2048
  finally:os.close(fd)
  old=os.open(b,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
  new=None
  try:
   assert os.write(old,b'old')==3
   with open(a,'wb') as f:assert f.write(b'new')==3
   os.replace(a,b)
   os.lseek(old,0,0);assert os.read(old,3)==b'old'
   assert os.fstat(old).st_nlink==0
   new=os.open(b,os.O_RDONLY);assert os.read(new,3)==b'new'
   os.unlink(b);assert os.fstat(new).st_nlink==0
  finally:
   if new is not None:os.close(new)
   os.close(old)
  if i%8==0:
   os.symlink('127.0.0.1:+123',link);os.unlink(link)
 os.rmdir(directory)

def main():
 with tempfile.TemporaryDirectory(prefix='a20-unlink-') as base:
  read,write=os.pipe();children=[]
  for index in range(4):
   pid=os.fork()
   if pid==0:
    os.close(write)
    try:
     assert os.read(read,1)==b'G';os.close(read)
     worker(base+'/'+str(index),160)
    except BaseException:
     traceback.print_exc();print('A20_TEST_FATAL: unlink worker '+str(index),flush=True);os._exit(1)
    os._exit(0)
   children.append(pid)
  os.close(read);assert os.write(write,b'GGGG')==4;os.close(write)
  failures=[]
  for pid in children:
   result=os.waitpid(pid,0);failures.append(result[1])
  assert failures==[0]*4,failures
 ring=subprocess.check_output(['dmesg','-s','1048576'],text=True)
 for marker in ('transaction poisoned','poison caller=','deferred orphan cleanup failed','orphan truncate failed'):
  assert marker not in ring,marker
 print('ORPHAN_STRESS: PASS 4 workers / 640 open-unlink-truncate-replace cycles',flush=True)
try:main()
except BaseException:
 print('A20_TEST_FATAL: orphan stress failed',flush=True)
 raise
