#!/usr/bin/env python3
"""Host-driven, one-port TCP server benchmark over identical QEMU hostfwd.

Reuses frozen inputs and base QEMU commands from bench-net-kvm.py manifests.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import time

spec=importlib.util.spec_from_file_location('netbench',Path(__file__).resolve().parents[1] / 'bench-net-kvm.py')
net=importlib.util.module_from_spec(spec);spec.loader.exec_module(net)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--lanes4',type=Path,required=True)
    p.add_argument('--lanes4q32',type=Path)
    p.add_argument('--sides',default='linux-kvm,a20-lanes1,a20-lanes4')
    p.add_argument('--runs',type=int,default=3)
    p.add_argument('--seconds',type=float,default=3)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);out=a.out.resolve()
    base=json.loads(a.baseline.read_text());paths={k:Path(v['path']) for k,v in base['inputs'].items()}
    manifest=dict(baseline=str(a.baseline.resolve()),seconds=a.seconds,runs=a.runs,samples=[],boots=[],created=time.strftime('%Y-%m-%dT%H:%M:%S%z'),lanes4_sha256=net.apk.sha(a.lanes4))
    if a.lanes4q32: manifest['lanes4q32_sha256']=net.apk.sha(a.lanes4q32)
    def save(): (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (out/'runner.py').write_text(Path(__file__).read_text());save()
    for run in range(a.runs):
        sides=a.sides.split(',');sides=sides[run%len(sides):]+sides[:run%len(sides)]
        for side in sides:
            linux=side=='linux-kvm'
            template=next(b['argv'] for b in base['boots'] if b['side']==('linux-kvm' if linux else 'a20-kvm'))
            argv=list(template);image=out/f'{run+1}-{side}.img'
            subprocess.run(['cp','--reflink=auto','--sparse=always',str(paths['world']),str(image)],check=True)
            for i,s in enumerate(argv):
                if 'id=xworld' in s: argv[i]=f'file={image},if=none,format=raw,id=xworld'
                if s=='user,id=net': argv[i]=s+',hostfwd=tcp:127.0.0.1:39051-:39051'
            if side=='a20-lanes4': argv[argv.index('-kernel')+1]=str(a.lanes4.resolve())
            if side=='a20-q32': argv[argv.index('-kernel')+1]=str(a.lanes4q32.resolve())
            lines=['#!/bin/sh','export PATH=/usr/sbin:/usr/bin:/sbin:/bin','sha256sum /usr/bin/net-microbench']
            if linux: lines+=['ip link set lo up','ip link set eth0 up','ip addr add 10.0.2.15/24 dev eth0','ip route add default via 10.0.2.2']
            else: lines+=['echo "tcpmode lwip" > /proc/net/config','echo 0 > /proc/a20/perf']
            lines+=['/usr/bin/net-microbench server tcp 39051 &','sleep 1']
            for mode in ['client','multi']:
                if not linux: lines+=['echo reset > /proc/a20/lock_contention']
                lines += [f'echo INBOUND_READY_{mode}',f'sleep {a.seconds+5}']
                if not linux: lines+=['cat /proc/a20/lock_contention','cat /proc/a20/netmem','cat /proc/net/dev','cat /proc/net/status']
                lines += [f'echo INBOUND_DONE_{mode}']
            lines+=['sleep 600']
            script=out/f'{run+1}-{side}.sh';script.write_text('\n'.join(lines)+'\n')
            net.install(image,paths['binary'],'/usr/bin/net-microbench');net.install(image,script,'/etc/a20-benchmark.sh')
            if linux: net.install(image,a.baseline.parent/'linux-init','/etc/a20-linux-init')
            manifest['boots'].append(dict(run=run+1,side=side,argv=argv));save()
            g=net.Guest(argv,out/f'{run+1}-{side}-serial.log')
            try:
                for mode in ['client','multi']:
                    before,_=g.until(f'(?:^|\n)INBOUND_READY_{mode}\r*\n'.encode(),20)
                    if mode=='client' and net.apk.sha(paths['binary']).encode() not in before: raise RuntimeError('binary hash mismatch')
                    cmd=['taskset','-c',base['affinity'],str(paths['binary']),mode,'tcp','39051','127.0.0.1','16384',str(a.seconds)]
                    t=time.monotonic();r=subprocess.run(cmd,capture_output=True,text=True,timeout=30);wall=time.monotonic()-t
                    output=r.stdout+r.stderr
                    raw,_=g.until(f'(?:^|\n)INBOUND_DONE_{mode}\r*\n'.encode(),20)
                    row=dict(run=run+1,side=side,case=mode,argv=cmd,rc=r.returncode,wall=wall,result=net.result(output),output=output,diagnostics=raw.decode(errors='replace').replace('\r',''))
                    manifest['samples'].append(row);save();print(run+1,side,mode,row['result'],flush=True)
            finally:g.close()
            image.unlink()

if __name__=='__main__':main()
