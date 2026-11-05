#!/usr/bin/env python3
"""Production Native identity/PID/object lifetime; hosted task/table adapters."""
import hashlib
import json
import argparse
from pathlib import Path
import subprocess
import shutil
root = Path(__file__).resolve().parent.parent.parent
out = root / '.kernel-build/cluster/native-task-object'
out.mkdir(parents=True, exist_ok=True)
report = out / 'report.json'
report.write_text(json.dumps(dict(passed=False))+'\n')
sources = ['kernel/proc/task_object.c', 'kernel/proc/pid.c', 'kernel/ipc/a20_object.c',
           'tools/tests/test_native_task_object.c']
shim = 'tools/tests/native_task_object_shim.h'
cases=[]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--implementations', nargs='+', choices=['c', 'rust'],
                    default=['c', 'rust'])
args = parser.parse_args()

arch_cases = [('x86_64','gcc',[],[]),
    ('aarch64','aarch64-linux-gnu-gcc',['qemu-aarch64'],[]),
    ('riscv64','riscv64-linux-gnu-gcc',['qemu-riscv64'],[]),
    ('x86_64-sanitize','gcc',[],['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'])]
rust_targets = {'x86_64':'x86_64-unknown-linux-gnu',
                'aarch64':'aarch64-unknown-none',
                'riscv64':'riscv64gc-unknown-linux-gnu',
                'x86_64-sanitize':'x86_64-unknown-linux-gnu'}

for implementation in args.implementations:
    if implementation == 'rust' and shutil.which('rustc') is None:
        raise RuntimeError('Rust task identity requested but rustc is unavailable')
    for arch, cc, runner, extra in arch_cases:
        if implementation == 'rust' and arch not in rust_targets:
            continue
        variant = f'{arch}-{implementation}'
        binary=out/variant
        command=[cc,'-std=gnu99','-Wall','-Wextra','-Werror','-O2','-pthread',
                 *([] if extra else ['-static']),*extra,
                 *(['-DCONFIG_TASK_OBJECT_RUST'] if implementation == 'rust' else []),
                 '-Ikernel/include','-Ikernel','-include',shim,*sources]
        if implementation == 'rust':
            rust_object = out / f'{variant}.o'
            rust_command = ['rustc','--edition=2021','--crate-type','lib','--emit=obj',
                '--target',rust_targets[arch],'-C','panic=abort','-C','opt-level=2',
                '-C','codegen-units=1','kernel/proc/task_object.rs','-o',str(rust_object)]
            subprocess.run(rust_command,cwd=root,check=True)
            command.append(str(rust_object))
        command += ['-o',str(binary)]
        subprocess.run(command,cwd=root,check=True)
        result=subprocess.run([*runner,str(binary)],cwd=root,text=True,
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
        (out/(variant+'.log')).write_text(result.stdout)
        assert result.returncode==0 and 'PASS Native task identity:' in result.stdout, result.stdout
        cases.append(dict(arch=arch,implementation=implementation,passed=True,
            command=command,runner=runner,rust_target=rust_targets.get(arch) if implementation == 'rust' else None,
            binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
        print('PASS Native task identity:',variant,flush=True)
report.write_text(json.dumps(dict(passed=True,cases=cases,
    evidence='same production identity API and fixture for C reference/Rust candidate, production PID allocator and generic object lifetime; hosted task/table/event/hardware adapters',
    sources={s:hashlib.sha256((root/s).read_bytes()).hexdigest() for s in sources+[shim,'kernel/include/proc/task_object.h','kernel/proc/task_object.rs']}),indent=2)+'\n')
