#!/usr/bin/env python3
"""Cold JBD2 revoke fixtures cross-checked by independent e2fsprogs replay."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/cli')]
from img import _crc32c
from a20_resource import Policy,Requirement,gate
from smoke import guest_slot_locks,_release_guest_slot_locks
import ext4_journal_gate as journal
from ext4_journal_io_gate import argv_for
HOME=b'HOME_REVOKED_BLOCK_OK\n'
LOG=b'LOG_APPLIED_BLOCK_OK\n'


def make_fixture(base,output,case):
    def bmap(path,logical):
        r=subprocess.run(['/usr/sbin/debugfs','-R',f'bmap {path} {logical}',str(base)],capture_output=True,text=True,check=True)
        return int(r.stdout.strip())
    mapping=[bmap('<8>',i) for i in range(8)]
    target=bmap('/test.txt',0)
    stat=subprocess.run(['/usr/sbin/debugfs','-R','stat /test.txt',str(base)],capture_output=True,text=True,check=True).stdout
    file_size=int(re.search(r'\bSize:\s*(\d+)',stat)[1])
    if file_size <= max(len(HOME),len(LOG)) or file_size > 4096:
        raise RuntimeError('test.txt must fit the single-block text fixture')
    def content(marker):
        # End the inode's actual bytes with a newline, so a fresh shell prompt
        # remains distinguishable from arbitrary file output.
        return (marker + b' '*(file_size-len(marker)-1) + b'\n').ljust(4096,b'\0')
    data=bytearray(base.read_bytes());block_size=1024<<struct.unpack_from('<I',data,1024+24)[0]
    if block_size!=4096:raise RuntimeError('requires 4 KiB journal fixture')
    j=bytearray(data[mapping[0]*4096:(mapping[0]+1)*4096]);uuid=j[48:64]
    seed=_crc32c(0xffffffff,uuid)
    first=0xffffffff if case=='wrapped-later-revoke' else 1
    struct.pack_into('>III',j,24,first,1,0)
    struct.pack_into('>I',j,40,0x13) # REVOKE | 64BIT | CSUM_V3
    struct.pack_into('>I',j,252,0);struct.pack_into('>I',j,252,_crc32c(0xffffffff,j[:1024]))
    data[mapping[0]*4096:(mapping[0]+1)*4096]=j
    data[target*4096:(target+1)*4096]=content(HOME)
    sb=bytearray(data[1024:2048]);struct.pack_into('<I',sb,96,struct.unpack_from('<I',sb,96)[0]|4)
    struct.pack_into('<I',sb,1020,_crc32c(0xffffffff,sb[:1020]));data[1024:2048]=sb
    def record(typ,seq):
        b=bytearray(4096);struct.pack_into('>III',b,0,0xc03b3998,typ,seq);return b
    def tail(b):
        struct.pack_into('>I',b,4092,_crc32c(seed,b));return b
    def write(seq):
        payload=content(LOG);d=record(1,seq)
        struct.pack_into('>IIII',d,12,target,8,0,_crc32c(_crc32c(seed,struct.pack('>I',seq)),payload))
        d[28:44]=uuid
        return [tail(d),payload]
    def revoke(seq,invalid=False):
        r=record(5,seq);struct.pack_into('>IQ',r,12,24,len(data)//4096 if invalid else target)
        return tail(r)
    def commit(seq):
        c=record(2,seq);struct.pack_into('>I',c,16,_crc32c(seed,c));return c
    next_seq=(first+1)&0xffffffff
    if case=='no-revoke':records=write(first)+[commit(first)];expected=LOG;tx,blocks,revokes,hits=1,1,0,0
    elif case in ('same-transaction-revoke','invalid-target'):
        records=write(first)+[revoke(first,case=='invalid-target'),commit(first)]
        expected=HOME;tx,blocks,revokes,hits=1,0,1,1
    elif case in ('later-revoke','wrapped-later-revoke'):
        records=write(first)+[commit(first),revoke(next_seq),commit(next_seq)]
        expected=HOME;tx,blocks,revokes,hits=2,0,1,1
    elif case=='older-revoke':
        records=[revoke(first),commit(first)]+write(next_seq)+[commit(next_seq)]
        expected=LOG;tx,blocks,revokes,hits=2,1,1,0
    else:raise RuntimeError(case)
    for i,b in enumerate(records,1):data[mapping[i]*4096:(mapping[i]+1)*4096]=b
    output.write_bytes(data)
    return target,expected,(tx,blocks,revokes,hits)


def checked_fsck(image,log,replay=False):
    command=['/usr/sbin/e2fsck','-fy' if replay else '-fn']
    if replay:command+=['-E','journal_only']
    result=subprocess.run(command+[str(image)],capture_output=True,text=True)
    log.write_text(result.stdout+result.stderr)
    if result.returncode not in ((0,1) if replay else (0,)):
        raise RuntimeError(f'e2fsck failed ({result.returncode}); {log}')


def run(args):
    policy=Policy.from_env();gate(Requirement(1024+policy.reserve_mem_mb,2,policy.min_disk_mb),policy,ROOT,wait=False,label='ext4 cold revoke replay')
    args.output.mkdir(parents=True,exist_ok=True);base=args.build_dir.resolve()
    report=dict(passed=False,cases=[],kernel_sha256=hashlib.sha256((base/'kernel.elf').read_bytes()).hexdigest())
    try:
        for case in ('no-revoke','same-transaction-revoke','later-revoke','older-revoke','wrapped-later-revoke','invalid-target'):
            with tempfile.TemporaryDirectory(prefix='a20-revoke-') as tmp:
                image=Path(tmp)/'image.img'
                target,expected,counts=make_fixture(base/'ext4-journal.img',image,case)
                before=hashlib.sha256(image.read_bytes()).hexdigest()
                if case!='invalid-target':
                    reference=Path(tmp)/'e2fsprogs.img';shutil.copyfile(image,reference)
                    checked_fsck(reference,args.output/(case+'-external-replay.log'),True)
                    checked_fsck(reference,args.output/(case+'-external.fsck'))
                    with reference.open('rb') as handle:handle.seek(target*4096);oracle=handle.read(4096)
                    if not oracle.startswith(expected):raise RuntimeError(case+': independent replay disagrees with fixture')
                log=args.output/(case+'-guest.log')
                journal.feed(argv_for(base,image),'cat /extra/test.txt\npoweroff\n',0,str(log),90)
                output=log.read_bytes()
                if case=='invalid-target':
                    if b'Refusing mount after journal recovery failure' not in output:raise RuntimeError('out-of-range revoke target was accepted')
                    if hashlib.sha256(image.read_bytes()).hexdigest()!=before:raise RuntimeError('rejected revoke image changed')
                else:
                    match=re.search(rb'replay complete transactions=(\d+) blocks=(\d+) revokes=(\d+) hits=(\d+)',output)
                    if not match or tuple(map(int,match.groups()))!=counts:raise RuntimeError(case+': wrong guest replay counts')
                    if expected not in output:raise RuntimeError(case+': guest file read disagreed with independent replay')
                    with image.open('rb') as handle:handle.seek(target*4096);actual=handle.read(4096)
                    if actual!=oracle:raise RuntimeError(case+': home block differs from independent replay')
                    checked_fsck(image,args.output/(case+'-guest.fsck'))
                report['cases'].append(dict(name=case,replay_counts=counts,preimage_sha256=before,
                    postimage_sha256=hashlib.sha256(image.read_bytes()).hexdigest()))
                (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
                print('EXT4_REVOKE: '+case+' PASS',flush=True)
        report['passed']=True
    except Exception as error:report['error']=str(error);raise
    finally:(args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    state,handles=guest_slot_locks()
    if state=='busy':print('EXT4_REVOKE: shared QEMU slot busy');return 75
    try:return run(args)
    except (RuntimeError,journal.GateError,subprocess.SubprocessError) as error:print('EXT4_REVOKE: FAIL '+str(error));return 1
    finally:
        if handles is not None:_release_guest_slot_locks(handles)
if __name__=='__main__':raise SystemExit(main())
