"""Compare the public process memory ABIs for a quiescent mapped child."""
import json
import mmap
import os
import re

ready_r, ready_w = os.pipe()
done_r, done_w = os.pipe()
pid = os.fork()
if pid == 0:
    os.close(ready_r)
    os.close(done_w)
    data = mmap.mmap(-1, 8 << 20)
    for offset in range(0, len(data), 4096):
        data[offset] = 1
    os.write(ready_w, b'R')
    os.read(done_r, 1)
    data.close()
    os._exit(0)
os.close(ready_w)
os.close(done_r)
try:
    assert os.read(ready_r, 1) == b'R'
    stat = open('/proc/%d/stat' % pid).read()
    tail = stat.rsplit(') ', 1)[1].split()
    field = lambda number: tail[number - 3]
    rss = int(field(24))
    virtual = int(field(23))
    statm = list(map(int, open('/proc/%d/statm' % pid).read().split()))
    status = open('/proc/%d/status' % pid).read()
    kb = int(re.search(r'^VmRSS:\s+(\d+) kB$', status, re.M)[1])
    assert len(tail) >= 37 and int(field(21)) == 0
    assert rss >= 2048 and rss * 4096 <= virtual, (rss, virtual)
    assert rss == statm[1] and virtual // 4096 == statm[0]
    assert rss * 4 == kb, (rss, kb)
    print('PROC_MEMORY: ' + json.dumps(dict(pid=pid, rss_pages=rss,
          virtual_bytes=virtual, status_kb=kb, processor=int(field(39)))), flush=True)
    print('PROC_MEMORY: PASS stat field 24, statm and VmRSS agree', flush=True)
finally:
    os.write(done_w, b'Q')
    os.close(done_w)
    os.close(ready_r)
    child, status = os.waitpid(pid, 0)
    assert child == pid and status == 0, (child, status)
