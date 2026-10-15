#!/usr/bin/env python3
import argparse, hashlib, json, os, re, selectors, signal, subprocess, time
from pathlib import Path

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1<<20),b''): h.update(b)
    return h.hexdigest()

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--kernel',type=Path,required=True)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--command',required=True)
    p.add_argument('--pass-marker',required=True)
    p.add_argument('--timeout',type=float,default=60)
    a=p.parse_args(); a.output.mkdir(parents=True,exist_ok=True)
    argv=['qemu-system-riscv32','-machine','virt','-m','1G','-serial','stdio','-monitor','none','-smp','1','-bios','default','-global','virtio-mmio.force-legacy=false','-drive',f'file={a.image},if=none,format=raw,id=x0','-device','virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0','-kernel',str(a.kernel)]
    pre=sha(a.image); data=bytearray(); sent=False; power=False; start=time.monotonic(); deadline=start+a.timeout
    prompt=re.compile(rb'(?:^|\r?\n)# $')
    with (a.output/'guest.log').open('wb',buffering=0) as log:
      proc=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,bufsize=0,start_new_session=True)
      sel=selectors.DefaultSelector(); sel.register(proc.stdout,selectors.EVENT_READ)
      while time.monotonic()<deadline:
        if proc.poll() is not None: break
        for key,_ in sel.select(.2):
          b=os.read(key.fileobj.fileno(),4096)
          if not b: continue
          log.write(b); data.extend(b)
          if not sent and prompt.search(data[-16384:]):
            proc.stdin.write((a.command+'\n').encode()); proc.stdin.flush(); sent=True
          if sent and not power and a.pass_marker.encode() in data:
            proc.stdin.write(b'poweroff\n'); proc.stdin.flush(); power=True
      timed=proc.poll() is None; forced=False
      if timed:
        forced=True; os.killpg(proc.pid,signal.SIGTERM)
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: os.killpg(proc.pid,signal.SIGKILL); proc.wait()
        rc=124
      else: rc=proc.returncode
    post=sha(a.image); text=bytes(data)
    forbidden=re.findall(rb'(?im)^.*(?:\bPANIC\b|\bFAULT\b|\bFAIL(?:ED)?\b|LOCK-STALL).*$' ,text)
    required=[a.pass_marker.encode(),b'System is going down for power-off NOW',b'[SD] audit errors=0']
    found=[x.decode() for x in required if x in text]
    passed=rc==0 and sent and power and len(found)==len(required) and not forbidden and pre==post
    report={'pass':passed,'exit':rc,'actual_timeout':timed,'forced_termination':forced,'arch':'riscv32','abi':'ilp32d','smp':1,'command':a.command,'pass_marker':a.pass_marker,'shell_prompt_seen':sent,'poweroff_after_marker':power,'required_found':found,'forbidden_found':[x.decode(errors='replace') for x in forbidden],'kernel_sha256':sha(a.kernel),'fat_pre_sha256':pre,'fat_post_sha256':post,'elapsed_seconds':round(time.monotonic()-start,3)}
    (a.output/'guest.argv.json').write_text(json.dumps({'argv':argv,'timeout_seconds':a.timeout},indent=2)+'\n')
    (a.output/'guest.exit').write_text(f'{rc}\n')
    (a.output/'guest.report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report)); return 0 if passed else 1
if __name__=='__main__': raise SystemExit(main())
