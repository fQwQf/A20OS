#!/usr/bin/env python3
"""Production Native identity/PID/object lifetime; hosted task/table adapters."""
import hashlib
import json
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parent.parent
out = root / '.kernel-build/cluster/native-task-object'
out.mkdir(parents=True, exist_ok=True)
report = out / 'report.json'
report.write_text(json.dumps(dict(passed=False))+'\n')
sources = ['kernel/proc/task_object.c', 'kernel/proc/pid.c', 'kernel/ipc/a20_object.c',
           'tools/tests/test_native_task_object.c']
shim = 'tools/tests/native_task_object_shim.h'
cases=[]
for arch, cc, runner, extra in [('x86_64','gcc',[],[]),
    ('aarch64','aarch64-linux-gnu-gcc',['qemu-aarch64'],[]),
    ('riscv64','riscv64-linux-gnu-gcc',['qemu-riscv64'],[]),
    ('x86_64-sanitize','gcc',[],['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'])]:
    binary=out/arch
    command=[cc,'-std=gnu99','-Wall','-Wextra','-Werror','-O2','-pthread',
             *([] if extra else ['-static']),*extra,'-Ikernel/include','-Ikernel',
             '-include',shim,*sources,'-o',str(binary)]
    subprocess.run(command,cwd=root,check=True)
    result=subprocess.run([*runner,str(binary)],cwd=root,text=True,
        stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
    (out/(arch+'.log')).write_text(result.stdout)
    assert result.returncode==0 and 'PASS Native task identity:' in result.stdout, result.stdout
    cases.append(dict(arch=arch,passed=True,command=command,runner=runner,
        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
    print('PASS Native task identity:',arch,flush=True)
report.write_text(json.dumps(dict(passed=True,cases=cases,
    evidence='production identity, PID allocator, generic object lifetime; hosted task/table/event/hardware adapters',
    sources={s:hashlib.sha256((root/s).read_bytes()).hexdigest() for s in sources+[shim,'kernel/include/proc/task_object.h']}),indent=2)+'\n')
