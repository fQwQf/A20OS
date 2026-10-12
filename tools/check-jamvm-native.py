#!/usr/bin/env python3
"""Execute upstream Native JamVM launcher and real JNI adapter probes in A20OS.

This gate proves launcher/ABI execution only. It does not accept Java, GC,
Minecraft, other architectures or physical boards. All runtime inputs freeze
before QEMU starts; the optional bootstrap diagnostic remains a separate result.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native_elf(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected ELF64 little endian: ' + str(path))
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[1] != 2 or header[2] != 243 or header[7] & 6 != 4:
        raise ValueError('expected RV64 double-float executable: ' + str(path))
    offset, size, count = header[5], header[9], header[10]
    if size != 56 or offset + size * count > len(data):
        raise ValueError('invalid ELF program header table')
    types = [struct.unpack_from('<I', data, offset + size * n)[0] for n in range(count)]
    if 0x6a20a200 not in types or 3 in types:
        raise ValueError('expected static A20 Native startup, no PT_INTERP')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--cycles', type=int, default=5)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--diagnose-bootstrap', action='store_true')
    parser.add_argument('--classlib', type=Path, help='freeze real class archives for bootstrap diagnosis')
    args = parser.parse_args()
    if args.classlib and not args.diagnose_bootstrap:
        parser.error('--classlib requires --diagnose-bootstrap')
    if args.cycles < 1 or args.timeout <= 0:
        parser.error('positive cycles and timeout required')
    output, build = args.output.resolve(), args.build.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('--output must be empty; retain existing evidence')
    output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, scope='launcher and JNI call adapter',
                  cpus=args.cpus, cycles=args.cycles, probes=[], java_application_verified=False,
                  gc_verified=False, minecraft_verified=False, physical_board=False)
    process = None
    phase = 'freeze'

    def save():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    save()
    try:
        manifest = json.loads((build / 'build.json').read_text())
        if not manifest['built']:
            raise ValueError('build did not complete')
        for name, key in [('jamvm.elf', 'program_sha256'), ('jni-probe.elf', 'jni_probe_sha256')]:
            if sha(build / name) != manifest[key]:
                raise ValueError('build/program checksum mismatch: ' + name)
            native_elf(build / name)
            shutil.copy2(build / name, output / name)
        shutil.copy2(build / 'build.json', output / 'build.json')
        shutil.copy2(args.kernel, output / 'kernel.elf')
        shutil.copy2(args.image, output / 'fat32.img')
        for name, dest in [('jamvm.elf', 'jamvm'), ('jni-probe.elf', 'jni-probe')]:
            subprocess.run(['mcopy', '-o', '-i', str(output / 'fat32.img'), str(output / name), '::/' + dest], check=True)
        class_inputs = []
        if args.classlib:
            classlib = args.classlib.resolve()
            class_manifest = json.loads((classlib / 'build.json').read_text())
            if not class_manifest['built']:
                raise ValueError('class library build did not complete')
            for name in ['glibj.zip', 'classes.zip']:
                if sha(classlib / name) != class_manifest['outputs'][name]:
                    raise ValueError('class library checksum mismatch: ' + name)
                shutil.copy2(classlib / name, output / name)
                subprocess.run(['mcopy', '-o', '-i', str(output / 'fat32.img'),
                                str(output / name), '::/' + name], check=True)
                class_inputs.append(name)
            shutil.copy2(classlib / 'build.json', output / 'classlib-build.json')
            class_inputs.append('classlib-build.json')
        flags = ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0', f'NR_CPUS={args.cpus}']
        if args.cpus > 1:
            flags += ['ALLOW_UNVERIFIED_SMP=1']
        command = shlex.split(subprocess.check_output(['make', '-s', *flags, '_qemu_argv'], cwd=ROOT, text=True))
        command[command.index('-kernel') + 1] = str(output / 'kernel.elf')
        command[command.index('-drive') + 1] = f'file={output / "fat32.img"},if=none,format=raw,id=x0'
        command += ['-snapshot']
        report['command'] = command
        report['inputs'] = {name: sha(output / name) for name in ['kernel.elf', 'fat32.img', 'jamvm.elf', 'jni-probe.elf', 'build.json'] + class_inputs}
        report['driver_sha256'] = sha(Path(__file__))
        probes = [('/bin/jni-probe', 'JAMVM_JNI: PASS actual adapter', 0),
                  ('/bin/jamvm -version', 'JamVM version 2.0.0', 0),
                  ('/bin/jamvm -fullversion', 'java full version "jamvm-1.5.0"', 0),
                  ('/bin/jamvm -help', 'Usage: /bin/jamvm', 0)] * args.cycles
        if args.diagnose_bootstrap:
            options = ' -Xbootclasspath/v:/bin/classes.zip -Xbootclasspath/c:/bin/glibj.zip -verbose:class' if args.classlib else ''
            probes.append(('/bin/jamvm' + options + ' A20MissingClass', None, None))
        save()
        index, start, prompts = 0, 0, 0
        with (output / 'run.log').open('wb') as stream:
            process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT)
            phase = 'boot'
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                content = (output / 'run.log').read_text(errors='replace')
                if '[PANIC]' in content or 'mlibc panic' in content or 'JAMVM_JNI: FAIL' in content or 'Library function fails due to missing sysdep' in content:
                    raise RuntimeError('runtime failure; see run.log')
                count = len(re.findall(r'(?:^|[\r\n])# ', content))
                if phase == 'boot' and count or phase == 'probe' and count > prompts:
                    if phase == 'probe':
                        segment = content[start:]
                        invocation, expected, status = probes[index]
                        result = re.search(r'(?:^|[\r\n])JAMVM_NATIVE_STATUS:(\d+)(?:[\r\n]|$)', segment)
                        if not result:
                            raise RuntimeError('missing numeric exit status')
                        actual = int(result[1])
                        if expected is not None:
                            if expected not in segment or actual != status:
                                raise RuntimeError('probe failed: ' + invocation)
                            report['probes'].append(dict(command=invocation, exit_status=actual))
                        else:
                            report['bootstrap_diagnostic'] = dict(command=invocation, exit_status=actual, output=segment,
                                accepted_as_java_execution=False)
                        print(f'RETURN {index + 1}: {invocation} status={actual}', flush=True)
                        index += 1
                        save()
                    if index == len(probes):
                        process.stdin.write(b'poweroff\n')
                        phase = 'shutdown'
                    else:
                        start, prompts = len(content), count
                        process.stdin.write((probes[index][0] + '; echo JAMVM_NATIVE_STATUS:$?\n').encode())
                        phase = 'probe'
                    process.stdin.flush()
                    deadline = time.monotonic() + args.timeout
                elif phase == 'shutdown' and 'System is going down for power-off NOW' in content:
                    phase = 'complete'
                    break
                if process.poll() is not None:
                    break
                time.sleep(0.05)
            if phase != 'complete':
                raise RuntimeError('stopped in ' + phase)
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
        report['phase'] = phase
        save()


if __name__ == '__main__':
    main()
