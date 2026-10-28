#!/usr/bin/env python3
"""Verified TCP/UDP echo microbenchmarks, host Linux / Linux KVM / A20OS KVM.

Requires a static Linux net-microbench ELF, existing benchmark world, and
Linux kernel/initrd. Makes private image copies; never mounts host filesystems.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('apk', ROOT / 'tools/bench-apk-linux.py')
apk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apk)

class Guest(apk.Guest):
    def until(self, pattern, timeout):
        return super().until(pattern.replace(rb'\r?\n', rb'\r*\n'), timeout)

def install(image, src, dst):
    subprocess.run(['/usr/sbin/debugfs', '-w', '-R', f'rm {dst}', str(image)], capture_output=True, check=True)
    subprocess.run(['/usr/sbin/debugfs', '-w', '-R', f'write {src} {dst}', str(image)], capture_output=True, check=True)
    actual = subprocess.check_output(['/usr/sbin/debugfs', '-R', f'cat {dst}', str(image)], stderr=subprocess.DEVNULL)
    if actual != src.read_bytes():
        raise RuntimeError(f'install mismatch {dst}')

def result(output):
    matches = re.findall(r'^RESULT (.+)$', output, re.M)
    if not matches:
        return None
    rows=[]
    for match in matches:
        d=dict(v.split('=') for v in match.split())
        rows.append({k:v if k=='protocol' else float(v) for k,v in d.items()})
    if len(rows)==1:
        return rows[0]
    parallel=re.search(r'PARALLEL clients=4 wall=([0-9.]+) verified=1',output)
    if len(rows)!=4 or not parallel:
        return None
    count=sum(r['count'] for r in rows); wall=float(parallel[1])
    return dict(protocol=rows[0]['protocol'],size=rows[0]['size'],count=count,
                seconds=wall,rps=count/wall,MiBps=count*rows[0]['size']/wall/1048576,
                verified=1,clients=4,individual=rows)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['out', 'binary', 'a20-kernel', 'fat', 'world', 'linux-kernel', 'linux-initrd']:
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--runs', type=int, default=3)
    p.add_argument('--seconds', type=float, default=2)
    p.add_argument('--cpus', default='4,6')
    p.add_argument('--cases',help='comma-separated case names; default all')
    p.add_argument('--timeout',type=float,default=25)
    p.add_argument('--profile', action='store_true')
    p.add_argument('--sides', default='linux-host,linux-kvm,a20-kvm')
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    out = args.out.resolve()
    paths = {n: getattr(args,n).resolve() for n in ['binary','a20_kernel','fat','world','linux_kernel','linux_initrd']}
    manifest = dict(created=time.strftime('%Y-%m-%dT%H:%M:%S%z'),
                    inputs={k:dict(path=str(v),sha256=apk.sha(v)) for k,v in paths.items()},
                    git_head=apk.command(['git','rev-parse','HEAD']),
                    git_status=apk.command(['git','status','--short']),
                    uname=apk.command(['uname','-a']), lscpu=apk.command(['lscpu']),
                    qemu_version=apk.command(['qemu-system-x86_64','--version']),
                    affinity=args.cpus, seconds=args.seconds, runs=args.runs,
                    profile=args.profile, samples=[], boots=[], order=[])
    (out/'runner.py').write_text(Path(__file__).read_text())
    (out/'microbench.c').write_text((ROOT/'tools/net-microbench.c').read_text())
    def save(): (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    save()
    prefix=['taskset','-c',args.cpus]
    # Host echo servers are pinned to the same CPU set as every client/QEMU.
    servers=[]
    try:
        for proto,port in [('tcp',39021),('udp',39022)]:
            f=(out/f'host-{proto}-server.log').open('w')
            s=subprocess.Popen(prefix+[str(paths['binary']),'server',proto,str(port)],stdout=subprocess.PIPE,stderr=f,text=True)
            servers.append((s,f))
            if s.stdout.readline().strip()!='SERVER_READY': raise RuntimeError('host server failed')
        cases=[('local-tcp-rr64','local','tcp',39031,64),
               ('local-tcp-echo16k','local','tcp',39032,16384),
               ('local-udp-rr64','local','udp',39033,64),
               ('external-tcp-rr64','client','tcp',39021,64),
               ('external-tcp-echo16k','client','tcp',39021,16384),
               ('external-udp-rr64','client','udp',39022,64),
               ('external-tcp-echo16k-p4','multi','tcp',39021,16384)]
        if args.cases:
            chosen=set(args.cases.split(','));cases=[c for c in cases if c[0] in chosen]
            if len(cases)!=len(chosen): raise ValueError('unknown or duplicate case')
        manifest['cases']=cases
        sides=args.sides.split(',')
        for run in range(args.runs):
            order=sides[run%len(sides):]+sides[:run%len(sides)]
            for side in order:
                manifest['order'].append([run+1,side]);save()
                if side=='linux-host':
                    for name,mode,proto,port,size in cases:
                        argv=prefix+[str(paths['binary']),mode,proto,str(port),'127.0.0.1',str(size),str(args.seconds)]
                        t=time.monotonic();r=subprocess.run(argv,capture_output=True,text=True,timeout=25);wall=time.monotonic()-t
                        output=r.stdout+r.stderr
                        row=dict(run=run+1,side=side,case=name,argv=argv,rc=r.returncode,wall=wall,result=result(output),output=output)
                        manifest['samples'].append(row);save();print(side,name,row['result'],flush=True)
                    continue
                image=out/f'{run+1}-{side}.img'
                subprocess.run(['cp','--reflink=auto','--sparse=always',str(paths['world']),str(image)],check=True)
                script=out/f'{run+1}-{side}.sh'
                lines=['#!/bin/sh','export PATH=/usr/sbin:/usr/bin:/sbin:/bin','uname -a','sha256sum /usr/bin/net-microbench']
                if side=='linux-kvm':
                    lines += ['ip link set lo up','ip link set eth0 up','ip addr add 10.0.2.15/24 dev eth0','ip route add default via 10.0.2.2']
                else:
                    lines += ['echo "tcpmode lwip" > /proc/net/config','cat /proc/net/config','cat /proc/net/status']
                for i,(name,mode,proto,port,size) in enumerate(cases):
                    ip='127.0.0.1' if mode=='local' else '10.0.2.2'
                    if side=='a20-kvm':
                        lines += [f'echo "tcpmode {"fast" if mode=="local" else "lwip"}" > /proc/net/config']
                        lines += ['echo reset > /proc/a20/lock_contention','echo reset > /proc/a20/perf',f'echo {1 if args.profile else 0} > /proc/a20/perf']
                    lines += [f'echo CASE_BEGIN_{i}',shlex.join(['/usr/bin/net-microbench',mode,proto,str(port),ip,str(size),str(args.seconds)]),f'echo CASE_END_{i} $?']
                    if side=='a20-kvm':
                        lines += [f'echo PROFILE_BEGIN_{i}','cat /proc/a20/lock_contention' if args.profile else 'true','cat /proc/a20/perf','echo 0 > /proc/a20/perf',f'echo PROFILE_END_{i}']
                if side=='a20-kvm': lines += ['cat /proc/a20/lock_contention','cat /proc/net/dev','cat /proc/a20/netmem','cat /proc/net/status']
                lines += ['echo NET_SUITE_DONE','sleep 600']
                script.write_text('\n'.join(lines)+'\n')
                install(image,paths['binary'],'/usr/bin/net-microbench')
                install(image,script,'/etc/a20-benchmark.sh')
                argv=prefix+['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-no-reboot','-m','2G','-smp','2','-display','none','-monitor','none','-serial','stdio','-snapshot','-kernel',str(paths['a20_kernel'] if side=='a20-kvm' else paths['linux_kernel']),'-drive',f'file={paths["fat"]},if=none,format=raw,id=x0','-device','virtio-blk-pci,drive=x0','-drive',f'file={image},if=none,format=raw,id=xworld','-device','virtio-blk-pci,drive=xworld','-netdev','user,id=net','-device','virtio-net-pci,netdev=net']
                if side=='linux-kvm':
                    init=out/'linux-init'
                    init.write_text('#!/bin/sh\nexport PATH=/usr/sbin:/usr/bin:/sbin:/bin\nmkdir -p /proc /sys /dev /run /tmp\nmount -t proc proc /proc\nmount -t sysfs sysfs /sys\nmount -t devtmpfs devtmpfs /dev\necho A20_BENCH_READY\n/bin/sh /etc/a20-benchmark.sh\nsleep 600\n')
                    init.chmod(0o755);install(image,init,'/etc/a20-linux-init')
                    argv+=['-initrd',str(paths['linux_initrd']),'-append','root=/dev/vdb rw init=/etc/a20-linux-init console=ttyS0 quiet net.ifnames=0']
                if side=='a20-kvm':
                    argv+=['-append','a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dhcp=0 a20.tcpmode=lwip']
                manifest['boots'].append(dict(side=side,run=run+1,argv=argv));save()
                guest=Guest(argv,out/f'{run+1}-{side}-serial.log')
                try:
                    for i,(name,*_) in enumerate(cases):
                        before,_=guest.until(f'(?:^|\n)CASE_BEGIN_{i}\r*\n'.encode(),20)
                        if i==0 and paths['binary'].name and apk.sha(paths['binary']).encode() not in before:
                            raise RuntimeError('guest binary fingerprint mismatch')
                        t=time.monotonic();raw,m=guest.until(f'(?:^|\n)CASE_END_{i} (\\d+)\r*\n'.encode(),args.timeout);wall=time.monotonic()-t
                        output=raw.decode(errors='replace').replace('\r','')
                        row=dict(run=run+1,side=side,case=name,rc=int(m[1]),wall=wall,result=result(output),output=output)
                        if side=='a20-kvm':
                            raw,_=guest.until(f'(?:^|\n)PROFILE_END_{i}\r*\n'.encode(),15)
                            row['profile']=raw.decode(errors='replace').replace('\r','')
                            state=re.search(r'collection_enabled_before_snapshot: ([01])',row['profile'])
                            row['collection_enabled']=int(state[1]) if state else None
                        manifest['samples'].append(row);save();print(side,name,row['result'],flush=True)
                    guest.until(b'(?:^|\n)NET_SUITE_DONE\r*\n',20)
                except Exception as exc:
                    manifest.setdefault('errors',[]).append(dict(run=run+1,side=side,case=name,error=repr(exc)))
                    save();raise
                finally: guest.close()
                image.unlink()
    finally:
        for s,f in servers:
            s.terminate();s.wait(timeout=5);f.close()
        save()

if __name__=='__main__': main()
