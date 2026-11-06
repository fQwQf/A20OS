#!/usr/bin/env python3
"""58 workloads; fork/wait latency, setup/total cost and concurrent throughput.

Use build-fork-exit-probes.sh to build --probe (fork-matrix). Inputs are hashed
before/after; images are disposable copies, and formal timing disables counters.
--diag is for attribution only, never merge its timings with formal samples.
"""
import argparse, importlib.util, json, re, shlex, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('kvm',ROOT/'tools/archive/bench-apk-kvm.py')
kvm=importlib.util.module_from_spec(spec);spec.loader.exec_module(kvm)
b=kvm.bench
p=argparse.ArgumentParser(description='Fork/wait/exit matrix; identical static probe on KVM Linux and A20.')
for option in ['image','fat','linux-kernel','linux-initrd','probe']:
    p.add_argument('--'+option,required=True)
p.add_argument('--cpus',default='4,6')
p.add_argument('--out',required=True);p.add_argument('--kernel',required=True);p.add_argument('--runs',type=int,default=3);p.add_argument('--smp',type=int,default=2);p.add_argument('--diag',action='store_true');p.add_argument('--a20-only',action='store_true');a=p.parse_args()
if a.runs < 1 or a.smp < 1:
    p.error('--runs and --smp must be positive')
out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=False)
micro=Path(a.probe).resolve()
base=Path(a.image).resolve()
fat=Path(a.fat).resolve()
lkernel=Path(a.linux_kernel).resolve()
linit=Path(a.linux_initrd).resolve()
matrix=[]
for mode in ['file','file-first','file-rw','anon','anon-sparse','shared']:
 for size in [1,16,64]:
  for pattern in ([0,1,2] if mode in ['file','file-first'] else [0,3] if mode in ['file-rw','anon','shared'] else [0]):
   matrix.append((f'{mode}-{size}-p{pattern}-w1',[mode,'20' if size==64 else '40',str(size),str(pattern),'1']))
for mode in ['fragmented','fd']:
 for size in ([1,4] if mode=='fragmented' else [64,512,4090]):
  matrix.append((f'{mode}-{size}',[mode,'40',str(size),'0','1']))
for mode in ['anon','file','file-first']:
 for workers in [2,4]:
  matrix.append((f'{mode}-16-p0-w{workers}',[mode,'30','16','0',str(workers)]))
for mode in ['burst-any','burst-pid']:
 for size in [8,32,128]:
  matrix.append((f'{mode}-{size}',[mode,'10',str(size),'0','1']))
matrix.append(('fork-exec',['exec','40','1','0','1']))
commands=[(name,['/usr/bin/fork-analysis-micro']+args) for name,args in matrix]
commands += [('stress-fork',['stress-ng','--fork','1','--timeout','3s','--metrics-brief'])]

manifest={'git_head':b.command(['git','rev-parse','HEAD']),'uname':b.command(['uname','-a']),'runs':a.runs,'smp':a.smp,'cpus':a.cpus,'diag':a.diag,'inputs':{str(f):b.sha(f) for f in [Path(a.kernel),base,fat,micro,lkernel,linit]},'samples':[],'boots':[]}
def save(): (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
def ctr(t): return {m[1]:int(m[2]) for m in re.finditer(r'^([A-Za-z0-9_]+): (\d+)\s*$',t,re.M)}
save()
for run in range(1,a.runs+1):
    sides=['a20'] if a.a20_only else (['linux','a20'] if run%2 else ['a20','linux'])
    for side in sides:
        private=out/f'{run}-{side}.img';subprocess.run(['cp','--reflink=auto','--sparse=always',str(base),str(private)],check=True)
        lines=['#!/bin/sh','export PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C','uname -a','sha256sum /usr/bin/fork-analysis-micro','echo INFO_DONE']
        for i,(name,cmd) in enumerate(commands):
            token=f'{run}_{i}'
            if side=='a20':
                lines += ['echo 0 > /proc/a20/perf']
                if a.diag: lines += ['echo 1 > /proc/a20/perf','echo reset > /proc/a20/lock_contention',f'echo PRE_{token}','cat /proc/a20/perf','cat /proc/stat',f'echo PRE_END_{token}']
            lines += [f'echo APK_BENCH_BEGIN_{token}',shlex.join(cmd),f'r=$?; echo APK_BENCH_END_{token} $r']
            if side=='a20':
                if a.diag: lines += [f'echo POST_{token}','cat /proc/a20/perf','cat /proc/stat','cat /proc/a20/lock_contention','echo 0 > /proc/a20/perf',f'echo POST_END_{token}']
                else: lines += b.counter_audit_script(token)
        lines += ['echo SUITE_DONE']
        script=out/f'{run}-{side}.sh';script.write_text('\n'.join(lines)+'\n')
        # Remove only paths in the private disposable filesystem.
        for src,dst in [(script,'/etc/a20-benchmark.sh'),(micro,'/usr/bin/fork-analysis-micro')]:
            subprocess.run(['/usr/sbin/debugfs','-w','-R',f'rm {dst}',str(private)],capture_output=True)
            kvm.install(private,src,dst)
        argv=['taskset','-c',a.cpus,'qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-no-reboot','-m','2G','-smp',str(a.smp),'-display','none','-monitor','none','-serial','stdio','-snapshot','-kernel',str(a.kernel if side=='a20' else lkernel),'-drive',f'file={fat},if=none,format=raw,id=x0','-device','virtio-blk-pci,drive=x0','-drive',f'file={private},if=none,format=raw,id=xworld','-device','virtio-blk-pci,drive=xworld','-netdev','user,id=net','-device','virtio-net-pci,netdev=net']
        if side=='linux':
            init=out/'linux-init';init.write_text(kvm.LINUX_INIT);init.chmod(0o755)
            subprocess.run(['/usr/sbin/debugfs','-w','-R','rm /etc/a20-linux-init',str(private)],capture_output=True)
            kvm.install(private,init,'/etc/a20-linux-init')
            argv += ['-initrd',str(linit),'-append','root=/dev/vdb rw init=/etc/a20-linux-init console=ttyS0 quiet transparent_hugepage=never']
        manifest['boots'].append({'run':run,'side':side,'argv':argv});save()
        guest=kvm.KVMGuest(argv,out/f'{run}-{side}-qemu.log')
        try:
            info,_=guest.until(rb'(?:^|\n)INFO_DONE\r?\n',30)
            assert (b.sha(micro)+'  /usr/bin/fork-analysis-micro').encode() in info
            for i,(name,cmd) in enumerate(commands):
                token=f'{run}_{i}'
                if side=='a20' and a.diag: pre,_=guest.until(f'(?:^|\n)PRE_END_{token}\\r?\\n'.encode(),30)
                output,rc,wall=guest.result(token,120)
                row={'run':run,'side':side,'case':name,'exit_code':rc,'output':output.replace('\r',''),'wall':wall}
                m=re.search(r'ns_per_op=([\d.]+) fork_ns=(\d+) wait_ns=(\d+)',output)
                if m:
                    row.update(ns_per_op=float(m[1]),fork_ns=int(m[2]),wait_ns=int(m[3]))
                    row['n']=int(re.search(r' n=(\d+)',output)[1])
                    for field in ['setup_ns','total_ns','throughput']:
                        value=re.search(field+r'=([\d.]+)',output)
                        if value: row[field]=float(value[1])
                else: row['ops_per_s']=b.metric('fork',output,3)
                if side=='a20':
                    if a.diag:
                        post,_=guest.until(f'(?:^|\n)POST_END_{token}\\r?\\n'.encode(),30)
                        before,after=ctr(pre.decode()),ctr(post.decode())
                        row.update(perf_delta={k:v-before.get(k,0) for k,v in after.items()},pre=pre.decode(),post=post.decode())
                    else: row.update(b.read_counter_audit(guest,token))
                manifest['samples'].append(row);save()
                print(run,side,name,rc,row.get('ns_per_op',row.get('ops_per_s')),flush=True)
                assert rc==0
                assert row.get('ns_per_op',row.get('ops_per_s')) is not None, name
                if side=='a20' and not a.diag: assert row['collection_enabled_after_case']==0
            guest.until(rb'(?:^|\n)SUITE_DONE\r?\n',30)
        finally: guest.close()
manifest['inputs_unchanged']=all(b.sha(f)==h for f,h in manifest['inputs'].items());save()
assert manifest["inputs_unchanged"], "benchmark input changed during measurement"
