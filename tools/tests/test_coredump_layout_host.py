#!/usr/bin/env python3
"""Run production core-layout tests and an x86_64 C/Rust Linux note consumer.

The consumer uses the actual trap export/adapter, Linux system headers and,
when installed, GDB. Other ports retain their existing layout tests.
"""

from __future__ import annotations

import pathlib
import argparse
import platform
import re
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/proc/coredump_layout.rs"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-dir', type=pathlib.Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="a20-coredump-layout-") as tmp:
        work = pathlib.Path(tmp)
        binary = work / "coredump-layout-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        subprocess.run([str(binary), "--nocapture"], check=True)
        if platform.machine() not in ('x86_64', 'amd64'):
            print('CORE_X86: skipped Linux x86_64 header consumer on this host')
            return 0
        def function(source, name):
            match = re.search(r'^(?:static )?(?:inline )?[^\n;]*\b' + name +
                              r'\([^;{}]*\)\s*\{.*?^\}', source, re.M | re.S)
            if match is None:
                raise AssertionError('missing production function ' + name)
            return match.group()
        arch = (ROOT / 'kernel/arch/x86_64/include/trap_frame.h').read_text()
        start = arch.index('typedef struct {')
        end = arch.index('trap_context_t;', start) + len('trap_context_t;')
        generated = (ROOT / 'tools/tests/coredump_x86_host.c').read_text()
        generated = generated.replace('/* TRAP_CONTEXT */', arch[start:end])
        generated = generated.replace('/* ARCH_EXPORT */',
            function(arch, 'arch_ptrace_export_regs') + '\n' +
            function(arch, 'arch_coredump_export_regs'))
        generated = generated.replace('/* NOTE_ADAPTER */', function(
            (ROOT / 'kernel/proc/coredump.c').read_text(), 'coredump_fill_note_input'))
        source = work / 'core.c'
        source.write_text(generated)
        rust = work / 'layout.rs'
        rust.write_text(SOURCE.read_text() +
            '\n#[panic_handler]\nfn test_panic(_: &core::panic::PanicInfo) -> ! { loop {} }\n')
        library = work / 'liblayout.a'
        subprocess.run(['rustc', '--edition=2021', '--crate-type=staticlib',
                        '-C', 'panic=abort', '-C', 'opt-level=2', str(rust),
                        '-o', str(library)], check=True)
        consumer = work / 'consumer'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT), '-idirafter', str(ROOT / 'kernel/include'),
                        str(source), str(library), '-o', str(consumer)], check=True)
        core = work / 'x86.core'
        subprocess.run([str(consumer), str(core)], check=True, timeout=10)
        if args.evidence_dir:
            args.evidence_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(core, args.evidence_dir / core.name)
        debugger = shutil.which('gdb')
        if debugger:
            result = subprocess.run([debugger, '-nx', '-batch', '-c', str(core),
                '-ex', 'printf "CORE_REGS %lx %lx %lx\\n", $rax, $rip, $rsp',
                '-ex', 'printf "CORE_TLS %lx %lx\\n", $fs_base, $gs_base'],
                check=True, capture_output=True, text=True, timeout=10)
            if args.evidence_dir:
                (args.evidence_dir / 'gdb.txt').write_text(result.stdout + result.stderr)
            if 'CORE_REGS 1000 1010 1007' not in result.stdout:
                raise AssertionError(result.stdout + result.stderr)
            if 'CORE_TLS 777000 888000' not in result.stdout:
                raise AssertionError(result.stdout + result.stderr)
            print('CORE_X86: GDB reads the emitted RAX/RIP/RSP and MSR-sourced TLS bases')
        else:
            print('CORE_X86: skipped debugger consumer (gdb unavailable)')
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
