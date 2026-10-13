#!/usr/bin/env python3
"""Build an isolated full Native libc from frozen source and cross configuration.

No system installation, default rootfs changes or downloads. Runtime acceptance
is separate; source, commands, compiler versions and sysroot hashes are retained.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def hashes(directory):
    return {str(p.relative_to(directory)): sha(p) for p in sorted(directory.rglob('*')) if p.is_file()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch', choices=['riscv64', 'aarch64'], default='riscv64')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--cc', help='optional target C compiler override')
    p.add_argument('--cxx', help='optional target C++ compiler override')
    args = p.parse_args()
    out = args.output.resolve()
    if args.jobs < 1 or (out.exists() and any(out.iterdir())):
        p.error('positive jobs and empty output required; preserve failed builds')
    out.mkdir(parents=True, exist_ok=True)
    report = dict(built=False, arch=args.arch, abi='a20-native', runtime_verified=False, commands=[])

    def save():
        (out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(command, phase):
        report['phase'] = phase
        report['commands'].append(dict(argv=command, log=phase + '.log'))
        save()
        print(phase, flush=True)
        with (out / (phase + '.log')).open('w') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)

    save()
    try:
        source = out / 'source'
        # Exclude only local build products and VCS state, never source files.
        shutil.copytree(ROOT / 'user/external/mlibc', source, symlinks=True,
                        ignore=shutil.ignore_patterns('.git', 'build', 'build-*', '__pycache__'))
        cross = source / 'ci' / ('a20-' + args.arch + '.cross-file')
        driver = out / 'build-mlibc-native.py'
        shutil.copy2(Path(__file__), driver)
        frozen = hashes(source)
        report['frozen_source'] = frozen
        report['driver_sha256'] = sha(driver)
        prefix = {'riscv64': 'riscv64-unknown-elf-', 'aarch64': 'aarch64-linux-gnu-'}[args.arch]
        compilers = {'c': args.cc or prefix + 'gcc', 'cpp': args.cxx or prefix + 'g++'}
        report['compilers'] = {key: dict(command=value,
            executable_sha256=sha(Path(shutil.which(value))),
            version=subprocess.check_output([value, '--version'], text=True).splitlines()[0])
            for key, value in compilers.items()}
        override = out / 'compiler.cross-file'
        override.write_text('[binaries]\n' + ''.join(key + ' = ' + repr(value) + '\n' for key, value in compilers.items()))
        report['compiler_override_sha256'] = sha(override)
        build = out / 'build'
        sysroot = out / 'sysroot'
        run(['meson', 'setup', str(build), str(source), '--cross-file', str(cross),
             '--cross-file', str(override), '--wrap-mode=nodownload', '-Ddefault_library=static',
             '-Dbuild_tests=false', '--prefix=' + str(sysroot)], 'configure')
        run(['ninja', '-C', str(build), '-j' + str(args.jobs)], 'compile')
        run(['meson', 'install', '-C', str(build), '--no-rebuild', '--quiet'], 'install')
        for name, digest in frozen.items():
            if sha(source / name) != digest:
                raise ValueError('frozen source changed while building: ' + name)
        if not (sysroot / 'lib/libc.a').is_file():
            raise ValueError('full static libc archive missing')
        report.update(built=True, phase='built', sysroot=hashes(sysroot))
        print('BUILT Native full libc ' + args.arch, flush=True)
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
