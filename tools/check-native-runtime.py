#!/usr/bin/env python3
"""Production runtime core/syscalls; hosted task/capability/usercopy/table adapters."""
import hashlib
import json
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parent.parent
out = root / '.kernel-build/cluster/native-runtime-unit'
out.mkdir(parents=True, exist_ok=True)
report = out / 'report.json'
report.write_text(json.dumps(dict(passed=False))+'\n')
sources = ['kernel/core/runtime.c', 'kernel/abi/native/sys_runtime.c',
           'tools/tests/test_native_runtime.c']
shim = 'tools/tests/native_runtime_shim.h'
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
    assert result.returncode==0 and 'PASS Native runtime:' in result.stdout, result.stdout
    cases.append(dict(arch=arch,passed=True,command=command,runner=runner,
        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
    print('PASS Native runtime:',arch,flush=True)
report.write_text(json.dumps(dict(passed=True,cases=cases,
    evidence='production runtime core and syscall bodies; hosted task/capability/table/usercopy/hardware adapters',
    sources={s:hashlib.sha256((root/s).read_bytes()).hexdigest() for s in sources+[shim,'tools/tests/native_task_object_shim.h','kernel/include/uapi/a20/runtime.h','kernel/include/core/runtime.h']}),indent=2)+'\n')
