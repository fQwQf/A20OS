#!/usr/bin/env python3
"""Compile actual pinned BSD objects without DPDK or host libc.

This is an out-of-tree port dependency probe, not a runnable backend. Successful
compilation never suppresses undefined symbols or supplies fake runtime stubs.
"""
import argparse
import concurrent.futures
import hashlib
import json
import re
from pathlib import Path
import subprocess
import tomllib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--cc', default='x86_64-linux-gnu-gcc')
    parser.add_argument('--emulated-tls', action='store_true',
                        help='require a compiler with -femulated-tls; never access user FS/TLS')
    parser.add_argument('--net-profile', type=int, choices=[1, 2, 3], default=2)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    port = root / 'kernel/net/backends/bsd'
    pin = tomllib.loads((port/'upstream.toml').read_text())
    selection = tomllib.loads((port/'port-sources.toml').read_text())
    sources = selection['sources']
    port_sources = selection.get('port_sources', [])
    bsd_port_sources = selection.get('bsd_port_sources', [])
    upstream = root / pin['path']
    git = lambda *a: subprocess.check_output(['git', '-C', str(upstream), *a], text=True).strip()
    if git('rev-parse', 'HEAD') != pin['revision'] or git('status', '--porcelain', '--untracked-files=all'):
        parser.error('F-Stack revision must be pinned and clean')
    if len(sources) != len(set(sources)) or len(port_sources + bsd_port_sources) != len(set(port_sources + bsd_port_sources)):
        parser.error('duplicate BSD source entries')
    for source in sources:
        if not (upstream/source).is_file() or not (upstream/source).resolve().is_relative_to(upstream.resolve()):
            parser.error(f'invalid BSD source: {source}')
    for source in port_sources + bsd_port_sources:
        if not (root/source).is_file() or not (root/source).resolve().is_relative_to(port.resolve()):
            parser.error(f'invalid A20OS port source: {source}')
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
             '-fno-stack-protector', '-mno-red-zone', '-mgeneral-regs-only', '-mcmodel=large', '-fPIC', '-nostdinc',
             '-U__linux__', '-U__linux', '-Ulinux', '-U__gnu_linux__',
             '-D_KERNEL', '-DFSTACK', f"-D__FreeBSD__={pin.get('freebsd_major', 14)}", '-DINET', '-DINET6',
             '-DHAVE_KERNEL_OPTION_HEADERS', '-include', str(upstream/'lib/opt/opt_global.h')]
    if args.emulated_tls:
        flags += ['-femulated-tls']
    flags += ['-ffunction-sections', '-fdata-sections']
    for directory in ['lib/include', 'lib', 'freebsd', 'lib/opt', 'freebsd/contrib/ck/include']:
        flags += ['-I' + str(upstream / directory)]
    generated = out / 'generated'
    generated.mkdir()
    for name in ['device_if', 'bus_if', 'linker_if']:
        subprocess.run(['awk', '-f', str(upstream/'freebsd/tools/makeobjops.awk'),
                        str(upstream/f'freebsd/kern/{name}.m'), '-h', *(['-c'] if name == 'linker_if' else [])], cwd=generated, check=True)
    flags += ['-I' + str(machine), '-I' + str(generated)]

    port_flags = ['-O2', '-std=gnu99', '-ffreestanding', '-fno-builtin',
                  '-fno-common', '-fno-stack-protector', '-mno-red-zone',
                  '-mgeneral-regs-only', '-mcmodel=large', '-fno-pie', '-nostdinc',
                  '-I' + str(root/'kernel/arch/x86_64/include'),
                  '-I' + str(root/'kernel/include'), '-I' + str(upstream/'lib')]

    port_flags += [f'-DCONFIG_NET_PROFILE={args.net_profile}']

    def compile_one(entry):
        source, owner = entry
        source_path = (upstream if owner == 'upstream' else out if owner == 'generated' else root)/source
        name = source.replace('/', '_')
        obj = out / (name + '.o')
        argv = [args.cc, *(port_flags if owner == 'a20' else flags), '-c', str(source_path), '-o', str(obj)]
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (name + '.log')).write_bytes(result.stdout)
        row = dict(source=source, owner=owner, source_sha256=hashlib.sha256(source_path.read_bytes()).hexdigest(),
                   argv=argv, rc=result.returncode)
        if result.returncode == 0:
            overrides = selection.get('object_symbol_overrides', {}).get(source, {})
            if overrides:
                subprocess.run(['objcopy', *['--redefine-sym=' + old + '=' + new
                                            for old, new in overrides.items()], str(obj)], check=True)
                row['object_symbol_overrides'] = overrides
            row['undefined'] = subprocess.check_output(['nm', '-u', str(obj)], text=True).splitlines()
        return row
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        rows = list(pool.map(compile_one, [(s, 'upstream') for s in sources] + [(s, 'a20') for s in port_sources] + [(s, 'a20-bsd') for s in bsd_port_sources] + [('generated/linker_if.c', 'generated')]))
    linked = None
    if all(row['rc'] == 0 for row in rows):
        objects = [str(out/(row['source'].replace('/', '_') + '.o')) for row in rows]
        result = subprocess.run(['ld', '-r', *objects, '-o', str(out/'bsd-core.o')],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out/'partial-link.log').write_bytes(result.stdout)
        linked = dict(rc=result.returncode)
        if result.returncode == 0:
            linked['undefined'] = subprocess.check_output(['nm', '-u', str(out/'bsd-core.o')], text=True).splitlines()
            # BSD's malloc/free/printf and kernel globals must never bind to
            # similarly named A20OS symbols with different signatures. Prefix
            # both defined and unresolved symbols; only explicit host imports
            # cross the namespace. This remains a partial object, not a boot.
            bridge = {
                'timekeeping_get_monotonic': 'timekeeping_get_monotonic',
                'timekeeping_get_realtime': 'timekeeping_get_realtime',
                'port_host_alloc': 'kmalloc_atomic', 'port_host_free': 'kfree',
                'port_host_copy': 'memcpy', 'port_host_zero': 'memset',
                'port_host_panic': 'panic',
                'port_host_current_task': 'proc_current',
                'port_host_log': 'klog_write', 'port_host_random': 'random_fill',
                'port_host_move': 'memmove', 'port_host_compare': 'memcmp',
                'port_host_nic_bind': 'a20_bsd_host_nic_bind',
                'port_host_nic_unbind': 'a20_bsd_host_nic_unbind',
                'port_host_nic_recv': 'a20_bsd_host_nic_recv',
                'port_host_nic_send': 'a20_bsd_host_nic_send',
                'port_host_nic_poll': 'a20_bsd_host_nic_poll',
                'port_host_nic_token_freed': 'a20_bsd_host_nic_token_freed',
                '_GLOBAL_OFFSET_TABLE_': '_GLOBAL_OFFSET_TABLE_',
            }
            mapping = out/'host-imports.map'
            mapping.write_text(''.join(f'a20_bsd_{name} {target}\n' for name, target in bridge.items()))
            prefixed = out/'bsd-core-isolated.o'
            subprocess.run(['objcopy', '--prefix-symbols=a20_bsd_',
                            str(out/'bsd-core.o'), str(prefixed)], check=True)
            subprocess.run(['objcopy', '--redefine-syms=' + str(mapping),
                            str(prefixed)], check=True)
            global_symbols = subprocess.check_output(
                ['nm', '-g', '--format=posix', str(prefixed)], text=True)
            unexpected = [line.split()[0] for line in global_symbols.splitlines()
                          if line and not line.split()[0].startswith('a20_bsd_')
                          and line.split()[0] not in bridge.values()]
            if unexpected:
                raise SystemExit('unisolated BSD symbols: ' + ', '.join(unexpected))
            for line in global_symbols.splitlines():
                fields = line.split()
                if fields and fields[0] in bridge.values() and fields[1] not in ('U', 'w'):
                    raise SystemExit('BSD defines a host import: ' + fields[0])
            linked['namespace_checked'] = True
            linked['host_imports'] = {k: v for k, v in bridge.items() if k != '_GLOBAL_OFFSET_TABLE_'}
            linked['isolated_undefined'] = subprocess.check_output(
                ['nm', '-u', str(prefixed)], text=True).splitlines()
            symbols = subprocess.check_output(['readelf', '-sW', str(prefixed)], text=True)
            (out/'isolated-symbols.txt').write_text(symbols)
            linked['tls_symbols'] = [line.strip() for line in symbols.splitlines()
                                     if ' TLS ' in line]
            relocations = subprocess.check_output(['readelf', '-rW', str(prefixed)], text=True)
            (out/'isolated-relocations.txt').write_text(relocations)
            if args.emulated_tls:
                if linked['tls_symbols'] or re.search(r'R_X86_64_(?:TLS|TPOFF|DTP|GOTTPOFF)', relocations):
                    raise SystemExit('native TLS remains in emulated BSD core')
                assembly = subprocess.check_output(['objdump', '-d', str(prefixed)], text=True)
                if re.search(r'%[fg]s:', assembly):
                    raise SystemExit('BSD core still accesses FS/GS')
                linked['no_native_tls_checked'] = True
            (out/'isolated-sections.txt').write_text(subprocess.check_output(
                ['readelf', '-SW', str(prefixed)], text=True))
    manifest = dict(net_profile=args.net_profile, emulated_tls=args.emulated_tls, revision=pin['revision'], source_manifest_sha256=hashlib.sha256((port/'port-sources.toml').read_bytes()).hexdigest(), partial_link=linked, compiler=subprocess.check_output([args.cc, '--version'], text=True), status='object-compilation-only-not-linked', sources=rows)
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    for row in rows:
        print(f"{'COMPILED' if row['rc'] == 0 else 'FAILED'} {row['source']}")
    if git('status', '--porcelain', '--untracked-files=all'):
        raise SystemExit('probe modified upstream')
    raise SystemExit(1 if any(row['rc'] for row in rows) or (linked and linked['rc']) else 0)


if __name__ == '__main__':
    main()
