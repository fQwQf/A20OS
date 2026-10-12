#!/usr/bin/env python3
"""Boot real RV64 A20OS and exercise real full-libc thread signal coordination.

This is optional QEMU evidence, not a JVM or physical MCU acceptance test.
Frozen kernel/image/program inputs allow exact before/after comparisons.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
PASS = 'MLIBC_SIGNALS: PASS target masks pending wait timeout suspend join'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--cycles', type=int, default=1)
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--program', type=Path, required=True)
    args = parser.parse_args()
    if min(args.cycles, args.jobs, args.timeout) <= 0 or bool(args.kernel) != bool(args.image):
        parser.error('positive cycles/jobs/timeout and paired --kernel/--image required')
    output = (ROOT / args.output).resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('output must be empty; never overwrite frozen evidence')
    output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, arch='riscv64', cpus=args.cpus, cycles=[],
                  physical_board=False, java_bytecode=False)

    def save():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def build(command, filename):
        with (output / filename).open('w') as log:
            subprocess.run(command, cwd=ROOT, check=True, stdout=log, stderr=subprocess.STDOUT)

    save()
    flags = ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0', f'NR_CPUS={args.cpus}']
    if args.cpus > 1:
        flags += ['ALLOW_UNVERIFIED_SMP=1']
    process = None
    phase = 'build'
    try:
        if not args.kernel:
            report['kernel_build'] = ['make', 'CCACHE=', *flags, f'-j{args.jobs}', 'dev-build']
            build(report['kernel_build'], 'kernel-build.log')
        command = shlex.split(subprocess.check_output(
            ['make', '-s', *flags, '_qemu_argv'], cwd=ROOT, text=True).strip())
        kernel = ROOT / (args.kernel or command[command.index('-kernel') + 1])
        image = ROOT / (args.image or command[command.index('-drive') + 1].split('file=', 1)[1].split(',', 1)[0])
        shutil.copy2(kernel, output / 'kernel.elf')
        shutil.copy2(image, output / 'fat32.img')
        shutil.copy2(ROOT / args.program, output / 'program.elf')
        subprocess.run(['mcopy', '-o', '-i', str(output / 'fat32.img'),
                        str(output / 'program.elf'), '::/mlibc-signals'], check=True)
        command[command.index('-kernel') + 1] = str(output / 'kernel.elf')
        command[command.index('-drive') + 1] = f'file={output / "fat32.img"},if=none,format=raw,id=x0'
        command += ['-snapshot']
        report['command'] = command
        report['provided_inputs'] = {name: str(value) for name, value in
            [('kernel', args.kernel), ('image', args.image), ('program', args.program)] if value}
        report['working_tree_hashes_are_not_frozen_build_provenance'] = bool(args.kernel or args.program)
        report['inputs'] = {name: sha(output / name) for name in ['kernel.elf', 'fat32.img', 'program.elf']}
        report['working_tree_source_sha256'] = {name: sha(ROOT / name) for name in [
            'kernel/proc/sched.c', 'kernel/proc/proc.c', 'kernel/proc/proc_internal.h',
            'kernel/abi/native/handle_table.c', 'kernel/abi/native/sys_task_signal.c',
            'kernel/include/uapi/a20/task_signal.h', 'kernel/include/proc/proc.h',
            'user/external/mlibc/sysdeps/a20/sysdeps.cpp',
            'user/external/mlibc/options/internal/generic/threads.cpp',
            'user/external/mlibc/options/posix/generic/posix_signal.cpp',
            'user/tests/test_mlibc_signals.c']}
        report['driver_sha256'] = sha(Path(__file__))
        save()
        log_path = output / 'run.log'
        with log_path.open('wb') as stream:
            process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE,
                                       stdout=stream, stderr=subprocess.STDOUT)
            phase = 'boot'
            deadline = time.monotonic() + args.timeout
            start, prompts = 0, 0
            while time.monotonic() < deadline:
                content = log_path.read_text(errors='replace')
                if '[PANIC]' in content or 'mlibc panic' in content or 'MLIBC_SIGNALS: FAIL' in content:
                    raise RuntimeError('runtime panic/failure; see run.log')
                prompt_count = len(re.findall(r'(?:^|[\r\n])# ', content))
                if phase == 'boot' and prompt_count:
                    start, prompts = len(content), prompt_count
                    process.stdin.write(b'/bin/mlibc-signals; echo MLIBC_SIGNALS_STATUS:$?\n')
                    process.stdin.flush()
                    phase = 'program'
                    deadline = time.monotonic() + args.timeout
                elif phase == 'program' and prompt_count > prompts:
                    segment = content[start:]
                    if PASS not in segment:
                        raise RuntimeError('program returned without passing; see run.log')
                    if not re.search(r'(?:^|[\r\n])MLIBC_SIGNALS_STATUS:0(?:[\r\n]|$)', segment):
                        raise RuntimeError('program exit status was not zero; see run.log')
                    report['cycles'].append(dict(exit_status=0))
                    save()
                    print(f'PASS Native signal cycle {len(report["cycles"])}', flush=True)
                    if len(report['cycles']) == args.cycles:
                        process.stdin.write(b'poweroff\n')
                        phase = 'shutdown'
                    else:
                        start, prompts = len(content), prompt_count
                        process.stdin.write(b'/bin/mlibc-signals; echo MLIBC_SIGNALS_STATUS:$?\n')
                    process.stdin.flush()
                    deadline = time.monotonic() + args.timeout
                elif phase == 'shutdown' and 'System is going down for power-off NOW' in content:
                    phase = 'complete'
                    break
                if process.poll() is not None:
                    break
                time.sleep(0.05)
            if phase != 'complete':
                raise RuntimeError(f'stopped in {phase}; see run.log')
        report['passed'] = True
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        if process:
            report['host_terminated'] = False
            if process.poll() is None:
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    report['host_terminated'] = True
                    process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            process.stdin.close()
            report['returncode'] = process.returncode
            if process.returncode != 0 or report['host_terminated']:
                report['passed'] = False
                report.setdefault('failure', 'QEMU did not exit normally')
        report['phase'] = phase
        save()
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
