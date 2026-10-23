#!/usr/bin/env python3
"""Check the published Native runtime wire layouts across all three bindings.

No linking, host headers, guest modification or network access. Constants are
ABI invariants from docs/native-abi/10-runtime-extensions.md, not host sizeofs.
"""
import json
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--aarch64-cxx', default='aarch64-linux-gnu-g++')
    args = parser.parse_args()
    common = [('socket_config',80), ('terminal_config',60), ('net_endpoint',148),
              ('net_sendmsg_args',72), ('net_recvmsg_args',80), ('file_lock_args',48)]
    profiles = [
        ('kernel','kernel/include/abi/native/types.h',True),
        ('sdk','user/liba20rt/a20_types.h',True),
        ('mlibc','user/external/mlibc/sysdeps/a20/a20.hpp',False),
    ]
    targets = [
        ('rv64','riscv64-unknown-elf-gcc',['-march=rv64gc','-mabi=lp64d']),
        ('rv32','riscv64-unknown-elf-gcc',['-march=rv32gc','-mabi=ilp32d']),
        ('aarch64','aarch64-linux-gnu-gcc',['-mno-outline-atomics']),
    ]
    results=[]
    with tempfile.TemporaryDirectory(prefix='a20-wire-layout-') as directory:
        for binding,header,is_c in profiles:
            suffix = '_t' if is_c else ''
            source = '#include "'+header+'"\n'
            if binding == 'kernel':
                source += '#include "ipc/start_info.h"\n#include "uapi/a20/thread_exit.h"\n#include "uapi/a20/exec.h"\n'
            if binding == 'sdk': source += '#include "user/liba20rt/a20_task.h"\n'
            assertion = '_Static_assert' if is_c else 'static_assert'
            for name,size in common + [('exit_args',40),('exec_bootstrap_args',64),('start_info',120)]:
                source += f'{assertion}(sizeof(a20_{name}{suffix}) == {size}, "{name} wire size");\n'
            for member,offset in [('addr_capacity',72),('out_flags',76)]:
                source += f'{assertion}(__builtin_offsetof(a20_net_recvmsg_args{suffix}, {member}) == {offset}, "v1 prefix/padding invariant");\n'
            source += f'{assertion}(__builtin_offsetof(a20_start_info{suffix}, bootstrap_data) == 104, "startup v1 prefix");\n'
            path=Path(directory)/(binding+('.c' if is_c else '.cpp')); path.write_text(source)
            for arch,compiler,flags in targets:
                # The mlibc header contains only freestanding C++ declarations;
                # use the C driver to avoid depending on a C++ standard library.
                arch_name = {'rv64':'riscv64','rv32':'riscv32','aarch64':'aarch64'}[arch]
                config = ['-Ikernel/arch/'+arch_name+'/include', '-DCONFIG_'+arch_name.upper(), '-DCONFIG_'+('32BIT' if arch == 'rv32' else '64BIT'), '-DNR_CPUS=2', '-DCONFIG_NR_CPUS=2']
                command=[compiler,*flags,*config,'-fsyntax-only','-ffreestanding','-nostdinc','-I.','-Ikernel/include','-isystem',subprocess.check_output([compiler,'-print-file-name=include'],text=True).strip(),'-std=c11' if is_c else '-std=c++20',str(path)]
                if not is_c:
                    command[0] = args.aarch64_cxx if arch == 'aarch64' else compiler.replace('gcc', 'g++')
                subprocess.run(command,cwd=ROOT,check=True)
                results.append(dict(binding=binding,arch=arch,passed=True))
    print(json.dumps(dict(passed=True,profiles=results),indent=2))

if __name__=='__main__': main()
