#!/usr/bin/env python3
"""Compile actual pinned BSD objects without DPDK or host libc.

This is an out-of-tree port dependency probe, not a runnable backend. Successful
compilation never suppresses undefined symbols or supplies fake runtime stubs.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import tomllib




def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--cc', default='x86_64-linux-gnu-gcc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    port = root / 'kernel/net/backends/bsd'
    pin = tomllib.loads((port/'upstream.toml').read_text())
    sources = tomllib.loads((port/'port-sources.toml').read_text())['sources']
    upstream = root / pin['path']
    git = lambda *a: subprocess.check_output(['git', '-C', str(upstream), *a], text=True).strip()
    if git('rev-parse', 'HEAD') != pin['revision'] or git('status', '--porcelain', '--untracked-files=all'):
        parser.error('F-Stack revision must be pinned and clean')
    if len(sources) != len(set(sources)):
        parser.error('duplicate BSD source entries')
    for source in sources:
        if not (upstream/source).is_file() or not (upstream/source).resolve().is_relative_to(upstream.resolve()):
            parser.error(f'invalid BSD source: {source}')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out/'port-sources.toml').write_bytes((port/'port-sources.toml').read_bytes())
    (out/'upstream.toml').write_bytes((port/'upstream.toml').read_bytes())
    (out/'probe.py').write_bytes(Path(__file__).read_bytes())
    machine = out / 'machine_include'
    machine.mkdir()
    for name, source in [('machine', 'amd64'), ('x86', 'x86')]:
        (machine / name).symlink_to(upstream / f'freebsd/{source}/include', target_is_directory=True)
    flags = ['-O2', '-std=gnu99', '-ffreestanding', '-fno-builtin', '-fno-common',
             '-fno-stack-protector', '-mno-red-zone', '-mgeneral-regs-only', '-mcmodel=large', '-fno-pie', '-nostdinc',
             '-U__linux__', '-U__linux', '-Ulinux', '-U__gnu_linux__',
             '-D_KERNEL', '-DFSTACK', '-D__FreeBSD__=14', '-DINET', '-DINET6',
             '-DHAVE_KERNEL_OPTION_HEADERS', '-include', str(upstream/'lib/opt/opt_global.h')]
    for directory in ['lib/include', 'lib', 'freebsd', 'lib/opt', 'freebsd/contrib/ck/include']:
        flags += ['-I' + str(upstream / directory)]
    generated = out / 'generated'
    generated.mkdir()
    for name in ['device_if', 'bus_if']:
        subprocess.run(['awk', '-f', str(upstream/'freebsd/tools/makeobjops.awk'),
                        str(upstream/f'freebsd/kern/{name}.m'), '-h'], cwd=generated, check=True)
    flags += ['-I' + str(machine), '-I' + str(generated)]

    def compile_one(source):
        name = source.replace('/', '_')
        obj = out / (name + '.o')
        argv = [args.cc, *flags, '-c', str(upstream/source), '-o', str(obj)]
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (name + '.log')).write_bytes(result.stdout)
        row = dict(source=source, source_sha256=hashlib.sha256((upstream/source).read_bytes()).hexdigest(),
                   argv=argv, rc=result.returncode)
        if result.returncode == 0:
            row['undefined'] = subprocess.check_output(['nm', '-u', str(obj)], text=True).splitlines()
        return row
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        rows = list(pool.map(compile_one, sources))
    linked = None
    if all(row['rc'] == 0 for row in rows):
        objects = [str(out/(row['source'].replace('/', '_') + '.o')) for row in rows]
        result = subprocess.run(['ld', '-r', *objects, '-o', str(out/'bsd-core.o')],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out/'partial-link.log').write_bytes(result.stdout)
        linked = dict(rc=result.returncode)
        if result.returncode == 0:
            linked['undefined'] = subprocess.check_output(['nm', '-u', str(out/'bsd-core.o')], text=True).splitlines()
    manifest = dict(revision=pin['revision'], source_manifest_sha256=hashlib.sha256((port/'port-sources.toml').read_bytes()).hexdigest(), partial_link=linked, compiler=subprocess.check_output([args.cc, '--version'], text=True), status='object-compilation-only-not-linked', sources=rows)
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    for row in rows:
        print(f"{'COMPILED' if row['rc'] == 0 else 'FAILED'} {row['source']}")
    if git('status', '--porcelain', '--untracked-files=all'):
        raise SystemExit('probe modified upstream')
    raise SystemExit(1 if any(row['rc'] for row in rows) or (linked and linked['rc']) else 0)


if __name__ == '__main__':
    main()
