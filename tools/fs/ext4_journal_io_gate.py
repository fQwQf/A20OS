#!/usr/bin/env python3
"""Crash after persistent device errors; recover the same journalled disk cold.

The fault boot must reject shutdown rather than claiming dirty metadata was
saved. Only that observed refusal permits the harness to cut emulated power.
Recovery runs without the fault and must shut down naturally with a clean fsck.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/cli')]
import ext4_journal_gate as journal
from a20_resource import Policy, Requirement, gate
from smoke import guest_slot_locks, _release_guest_slot_locks
TOKEN=b'IO_DURABLE_001'
WRITE='echo "IO_"\'DURABLE_001\' > /extra/io.txt\nsync\npoweroff\n'
READ='cat /extra/io.txt\npoweroff\n'


def fault_boot(argv, log, timeout, require_retries=True):
    output=bytearray();pending=WRITE.splitlines();cursor=queries=0
    last=time.monotonic()
    with subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT,bufsize=0) as proc:
        with selectors.DefaultSelector() as poller:
            poller.register(proc.stdout,selectors.EVENT_READ)
            deadline=time.monotonic()+timeout
            try:
                while time.monotonic()<deadline and proc.poll() is None:
                    for key,_ in poller.select(.03):
                        data=os.read(key.fd,65536)
                        if data:output.extend(data);last=time.monotonic()
                        else:poller.unregister(key.fileobj)
                    if re.search(rb'PANIC|UBSAN:|Kernel BUG|LOCK-STALL',output):
                        raise RuntimeError('fault boot kernel diagnostic failed')
                    seen=output.count(b'\x1b[6n')
                    if seen>queries and proc.poll() is None:
                        proc.stdin.write(b'\x1b[1;1R'*(seen-queries));proc.stdin.flush();queries=seen
                    if b'[SD] poweroff refused: filesystem shutdown failed' in output:
                        if b'[EXT4/JBD2] commit failed:' not in output or b'transaction stays pending' not in output:
                            raise RuntimeError('shutdown refusal did not retain the journal transaction')
                        if require_retries and not re.search(rb'I/O error:.*after 3 retries',output):
                            raise RuntimeError('device error did not exhaust the real driver retries')
                        if not re.search(rb'bad completion head=\d+ status=1',output):
                            raise RuntimeError('missing actual device error completion')
                        if b'[SD] POWER_OFF entry' in output:
                            raise RuntimeError('fault boot incorrectly claimed shutdown completion')
                        proc.kill();proc.wait() # explicit power loss after observed refusal
                        return bytes(output)
                    if pending and time.monotonic()-last>=.05 and re.search(rb'(?:^|[\r\n])# ',output[cursor:]):
                        cursor=len(output);proc.stdin.write((pending.pop(0)+'\n').encode());proc.stdin.flush()
                raise RuntimeError('missing explicit failed-shutdown boundary before exit/timeout')
            finally:
                if proc.poll() is None:proc.kill();proc.wait()
                log.write_bytes(output)


def argv_for(base,image,rule=None):
    argv=journal.QEMU['riscv64'](str(base/'kernel.elf'),str(base/'fat32.img'),str(image),[])
    argv[argv.index('-smp')+1]='2'
    i=argv.index('-drive');argv[i+1]+=',snapshot=on'
    if rule:
        i=argv.index('-drive',i+2)
        argv[i:i+2]=['-blockdev',json.dumps(dict(driver='blkdebug',**{'node-name':'xe'},
            image=dict(driver='file',filename=str(image)),**{'inject-error':[rule]}))]
    return argv


def fsck(image,log,required):
    r=subprocess.run(['/usr/sbin/e2fsck','-fn',str(image)],capture_output=True,text=True)
    log.write_text(r.stdout+r.stderr)
    if required and r.returncode:raise RuntimeError(f'cold recovery fsck failed ({r.returncode}); {log}')
    return r.returncode


def run(args):
    policy=Policy.from_env();gate(Requirement(1024+policy.reserve_mem_mb,2,policy.min_disk_mb),
        policy,ROOT,wait=False,label='ext4 journal device I/O errors')
    args.output.mkdir(parents=True,exist_ok=True)
    base=args.build_dir.resolve();pristine=base/'ext4-journal.img'
    report=dict(passed=False,cases=[],kernel_sha256=hashlib.sha256((base/'kernel.elf').read_bytes()).hexdigest(),
                image_sha256=hashlib.sha256(pristine.read_bytes()).hexdigest())
    # Ask debugfs for physical journal blocks; no assumption of contiguous extents.
    def physical(logical):
        r=subprocess.run(['/usr/sbin/debugfs','-R',f'bmap <8> {logical}',str(pristine)],capture_output=True,text=True,check=True)
        return int(r.stdout.strip())
    # A control commit determines the actual number of metadata payload blocks.
    with tempfile.TemporaryDirectory(prefix='a20-journal-io-') as tmp:
        image=Path(tmp)/'ext4.img';shutil.copyfile(pristine,image)
        journal.feed(argv_for(base,image),WRITE,0,str(args.output/'control-write.log'),args.timeout)
        text=(args.output/'control-write.log').read_bytes()
        match=re.search(rb'commit 1: (\d+) metadata blocks',text)
        if not match:raise RuntimeError('missing control transaction size')
        count=int(match[1]);fsck(image,args.output/'control.fsck',True)
    stats=subprocess.run(['/usr/sbin/debugfs','-R','stats',str(pristine)],capture_output=True,text=True,check=True).stdout
    bitmap=re.search(r'Group\s+0: block bitmap at (\d+)',stats)
    if not bitmap or re.search(r'Group\s+1:',stats):
        raise RuntimeError('this gate requires a single-group 4 KiB journal fixture')
    rules=[('ordered-flush',None,False),('descriptor',physical(1)*8,False),('payload',physical(2)*8,False),
           ('commit',physical(2+count)*8,False),('checkpoint-bitmap',int(bitmap[1])*8,True)]
    try:
        for name,sector,survives in rules:
            with tempfile.TemporaryDirectory(prefix='a20-journal-io-') as tmp:
                image=Path(tmp)/'ext4.img';shutil.copyfile(pristine,image)
                rule=dict(event='none' if sector is None else 'pwritev',
                          iotype='flush' if sector is None else 'write',errno=5,once=False)
                if sector is not None:rule['sector']=sector
                argv=argv_for(base,image,rule)
                (args.output/(name+'-argv.json')).write_text(json.dumps(argv,indent=2)+'\n')
                fault_boot(argv,args.output/(name+'-fault.log'),args.timeout,require_retries=not survives)
                shutil.copyfile(image,args.output/(name+'-pre.img'))
                before=hashlib.sha256(image.read_bytes()).hexdigest()
                pre_fsck=fsck(image,args.output/(name+'-pre.fsck'),False)
                journal.feed(argv_for(base,image),READ,0,str(args.output/(name+'-recover.log')),args.timeout)
                shutil.copyfile(image,args.output/(name+'-post.img'))
                output=(args.output/(name+'-recover.log')).read_bytes()
                if (TOKEN in output)!=survives:raise RuntimeError(f'{name}: wrong commit survival boundary')
                if b'[EXT4] Mounted' not in output or b'Refusing mount' in output:raise RuntimeError(f'{name}: cold mount failed')
                if survives and b'replay complete' not in output:raise RuntimeError(f'{name}: missing committed replay')
                fsck(image,args.output/(name+'-post.fsck'),True)
                report['cases'].append(dict(name=name,sector=sector,survives=survives,
                    power_cut='after explicit shutdown refusal',pre_fsck_status=pre_fsck,
                    preimage_sha256=before,postimage_sha256=hashlib.sha256(image.read_bytes()).hexdigest()))
                (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
                print('EXT4_JOURNAL_IO: '+name+' PASS',flush=True)
        report['passed']=True
    except Exception as error:
        report['error']=str(error);raise
    finally:
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--timeout',type=float,default=90)
    args=p.parse_args()
    if args.timeout<=0:p.error('timeout must be positive')
    state,handles=guest_slot_locks()
    if state=='busy':print('EXT4_JOURNAL_IO: shared QEMU slot busy');return 75
    try:return run(args)
    except (RuntimeError,journal.GateError,subprocess.SubprocessError) as error:
        print('EXT4_JOURNAL_IO: FAIL '+str(error));return 1
    finally:
        if handles is not None:_release_guest_slot_locks(handles)

if __name__=='__main__':raise SystemExit(main())
