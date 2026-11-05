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
import shutil
from pathlib import Path
import subprocess
import tomllib


TARGETS = {
    'x86_64': dict(machine='amd64', triple='x86_64-linux-gnu',
                   cflags=['-mno-red-zone', '-mgeneral-regs-only', '-mcmodel=large']),
    'aarch64': dict(machine='arm64', triple='aarch64-linux-gnu',
                    cflags=['-march=armv8-a', '-mgeneral-regs-only',
                            '-mno-outline-atomics', '-mcmodel=large']),
    # The network personality reuses the vendored LP64 data definitions.
    # All machine operations and ELF identities are supplied by our overlay;
    # no ARM instructions or hardware CPU context cross this boundary.
    'riscv64': dict(machine='arm64', triple='riscv64-linux-gnu',
                    cflags=['-march=rv64imac_zicsr_zifencei', '-mabi=lp64',
                            '-mcmodel=medany', '-DA20_BSD_PORTABLE_LP64=1']),
    'loongarch64': dict(machine='arm64', triple='loongarch64-linux-gnu',
                        cflags=['-march=loongarch64', '-mabi=lp64d',
                                '-mno-lsx', '-mno-lasx', '-DA20_BSD_PORTABLE_LP64=1']),

}


def prepare_port_sources(upstream, out, sources, explicit_tls):
    """Apply checked port overlays to generated copies of the pinned input.

    Its FreeBSD personality has one exported TLS variable, pcurthread. Only
    the declaration/definition change; every read/write still calls the same
    task-owned emutls allocator. Keep the original submodule untouched, retain
    generated input and hashes, and fail if an upstream change invalidates the
    expected declarations. Quoted header lookups require the lib source copies
    and ff_dpdk_if.h to sit alongside the overlaid ff_api.h.
    """
    stage = out/'port-inputs/lib'
    stage.mkdir(parents=True)
    for source in sources:
        if source.startswith('lib/'):
            shutil.copyfile(upstream/source, stage/Path(source).name)
    rewrites = {
        'ff_api.h': ('extern __thread struct thread *pcurthread;',
                     '#include <a20_bsd_explicit_tls.h>'),
        'ff_compat.c': ('__thread struct thread *pcurthread = NULL;',
                        'struct port_emutls_control port_current_thread_control = '
                        '{sizeof(struct thread *), __alignof__(struct thread *), 0, NULL};'),
    }
    if explicit_tls:
        shutil.copyfile(upstream/'lib/ff_dpdk_if.h', stage/'ff_dpdk_if.h')
        for filename, (old, new) in rewrites.items():
            text = (upstream/'lib'/filename).read_text()
            if text.count(old) != 1:
                raise SystemExit(f'F-Stack explicit TLS declaration changed: {filename}')
            (stage/filename).write_text(text.replace(old, new))

    # UMA may choose a multi-page bootstrap slab (notably on arm64). Its
    # vsetzoneslab() then needs this table during uma_startup1(), not after it.
    # kmem_malloc uses the host backing allocator and does not require UMA.
    path = stage/'ff_freebsd_init.c'
    text = path.read_text()
    late_hash = '''    num_hash_buckets = 8192;
    uma_page_slab_hash = (struct uma_page_head *)kmem_malloc(sizeof(struct uma_page)*num_hash_buckets, M_ZERO);
    uma_page_mask = num_hash_buckets - 1;'''
    before_uma = '    uma_startup1((vm_offset_t)bootmem);'
    if text.count(late_hash) != 1 or text.count(before_uma) != 1:
        raise SystemExit('F-Stack UMA bootstrap ordering changed; review port overlay')
    early_hash = late_hash.replace('sizeof(struct uma_page)', 'sizeof(*uma_page_slab_hash)')
    text = text.replace(late_hash, '').replace(before_uma,
        early_hash + '\n    if (!bootmem || !uma_page_slab_hash) return ENOMEM;\n' + before_uma)
    path.write_text(text)

    # This backend statically links the network personality. Vendored dynamic
    # relocation code implements x86-64 only; reject requests on other targets
    # explicitly instead of interpreting another architecture's relocations.
    path = stage/'ff_compat.c'
    text = path.read_text()
    start = '    Elf64_Addr *where, val;'
    end = '    return(0);\n}\n\nint\nelf_reloc('
    if text.count(start) != 1 or text.count(end) != 1:
        raise SystemExit('F-Stack ELF relocation implementation changed')
    text = text.replace(start, '#ifndef __x86_64__\n    return ENOEXEC;\n#else\n' + start)
    text = text.replace(end, '    return(0);\n#endif\n}\n\nint\nelf_reloc(')
    path.write_text(text)

    return stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--cc', help='compiler (defaults to the target GCC; running kernels use Clang)')
    parser.add_argument('--arch', choices=TARGETS, default='x86_64')
    parser.add_argument('--cross-prefix', help='target binutils prefix (defaults to the target triple)')
    parser.add_argument('--emulated-tls', action='store_true',
                        help='require a compiler with -femulated-tls; never access user FS/TLS')
    parser.add_argument('--net-profile', type=int, choices=[1, 2, 3], default=2)
    parser.add_argument('--nr-cpus', type=int, default=1)
    args = parser.parse_args()
    target = TARGETS[args.arch]
    if args.cc is None:
        args.cc = target['triple'] + '-gcc'
    if args.nr_cpus < 1:
        parser.error('--nr-cpus must be positive')
    cross = args.cross_prefix if args.cross_prefix is not None else target['triple'] + '-'
    compiler_version = subprocess.check_output([args.cc, '--version'], text=True)
    target_flags = (['--target=' + target['triple']] if 'clang' in compiler_version.lower() else [])
    target_flags += target['cflags']
    root = Path(__file__).resolve().parents[2]
    port = root / 'kernel/net/backends/bsd'
    pin = tomllib.loads((port/'upstream.toml').read_text())
    selection = tomllib.loads((port/'port-sources.toml').read_text())
    checksum = ('freebsd/amd64/amd64/in_cksum.c' if args.arch == 'x86_64'
                else 'freebsd/arm64/arm64/in_cksum.c')
    sources = [s.replace('freebsd/amd64/amd64/in_cksum.c', checksum)
               for s in selection['sources']]
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
    for name, source in [('machine', target['machine']), ('x86', 'x86')]:
        (machine / name).symlink_to(upstream / f'freebsd/{source}/include', target_is_directory=True)
    flags = ['-O2', '-std=gnu99', '-ffreestanding', '-fno-builtin', '-fno-common',
             '-fno-stack-protector', *target_flags,
             *(['-fPIC'] if args.arch == 'x86_64' else ['-fno-pie']), '-nostdinc',
             '-U__linux__', '-U__linux', '-Ulinux', '-U__gnu_linux__',
             '-D_KERNEL', '-DFSTACK', f"-D__FreeBSD__={pin.get('freebsd_major', 14)}", '-DINET', '-DINET6',
             '-DHAVE_KERNEL_OPTION_HEADERS', '-include', str(upstream/'lib/opt/opt_global.h')]
    explicit_tls = args.emulated_tls and args.arch == 'loongarch64'
    staged_lib = prepare_port_sources(upstream, out, sources, explicit_tls)
    flags += ['-I' + str(staged_lib)]
    if explicit_tls:
        flags += ['-DA20_BSD_EXPLICIT_TLS=1']
    elif args.emulated_tls:
        flags += ['-femulated-tls']
    flags += ['-ffunction-sections', '-fdata-sections']
    if args.arch in ('riscv64', 'loongarch64'):
        flags += [f'-DMACHINE="{args.arch}"', f'-DMACHINE_ARCH="{args.arch}"']
    flags += ['-I' + str(port/'include')]
    for directory in ['lib/include', 'lib', 'freebsd', 'lib/opt', 'freebsd/contrib/ck/include']:
        flags += ['-I' + str(upstream / directory)]
    generated = out / 'generated'
    generated.mkdir()
    for name in ['device_if', 'bus_if', 'linker_if']:
        subprocess.run(['awk', '-f', str(upstream/'freebsd/tools/makeobjops.awk'),
                        str(upstream/f'freebsd/kern/{name}.m'), '-h', *(['-c'] if name == 'linker_if' else [])], cwd=generated, check=True)
    flags += ['-I' + str(machine), '-I' + str(generated)]

    port_flags = ['-O2', '-std=gnu99', '-ffreestanding', '-fno-builtin',
                  '-fno-common', '-fno-stack-protector', *target_flags,
                  '-fno-pie', '-nostdinc',
                  '-I' + str(root/f'kernel/arch/{args.arch}/include'),
                  '-I' + str(root/'kernel/include'),
                  '-I' + str(root/'kernel/net/backends/bsd'),
                  '-I' + str(upstream/'lib')]

    port_flags += [f'-DCONFIG_NET_PROFILE={args.net_profile}',
                   f'-DCONFIG_NR_CPUS={args.nr_cpus}']

    def compile_one(entry):
        source, owner = entry
        source_path = (upstream if owner == 'upstream' else out if owner == 'generated' else root)/source
        input_path = (staged_lib/Path(source).name
                      if staged_lib and owner == 'upstream' and source.startswith('lib/') else source_path)
        name = source.replace('/', '_')
        obj = out / (name + '.o')
        argv = [args.cc, *(port_flags if owner == 'a20' else flags), '-c', str(input_path), '-o', str(obj)]
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (name + '.log')).write_bytes(result.stdout)
        row = dict(source=source, owner=owner, source_sha256=hashlib.sha256(source_path.read_bytes()).hexdigest(),
                   compiled_source_sha256=hashlib.sha256(input_path.read_bytes()).hexdigest(),
                   argv=argv, rc=result.returncode)
        if result.returncode == 0:
            overrides = selection.get('object_symbol_overrides', {}).get(source, {})
            if overrides:
                subprocess.run([cross + 'objcopy', *['--redefine-sym=' + old + '=' + new
                                            for old, new in overrides.items()], str(obj)], check=True)
                row['object_symbol_overrides'] = overrides
            row['undefined'] = subprocess.check_output([cross + 'nm', '-u', str(obj)], text=True).splitlines()
        return row
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        rows = list(pool.map(compile_one, [(s, 'upstream') for s in sources] + [(s, 'a20') for s in port_sources] + [(s, 'a20-bsd') for s in bsd_port_sources] + [('generated/linker_if.c', 'generated')]))
    linked = None
    if all(row['rc'] == 0 for row in rows):
        objects = [str(out/(row['source'].replace('/', '_') + '.o')) for row in rows]
        result = subprocess.run([cross + 'ld', '-r', *objects, '-o', str(out/'bsd-core.o')],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out/'partial-link.log').write_bytes(result.stdout)
        linked = dict(rc=result.returncode)
        if result.returncode == 0:
            linked['undefined'] = subprocess.check_output([cross + 'nm', '-u', str(out/'bsd-core.o')], text=True).splitlines()
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
                'a20_bsd_netfilter_input': 'a20_bsd_netfilter_input',
                'a20_bsd_netfilter_output': 'a20_bsd_netfilter_output',
                '_GLOBAL_OFFSET_TABLE_': '_GLOBAL_OFFSET_TABLE_',
            }
            mapping = out/'host-imports.map'
            mapping.write_text(''.join(f'a20_bsd_{name} {target}\n' for name, target in bridge.items()))
            prefixed = out/'bsd-core-isolated.o'
            subprocess.run([cross + 'objcopy', '--prefix-symbols=a20_bsd_',
                            str(out/'bsd-core.o'), str(prefixed)], check=True)
            subprocess.run([cross + 'objcopy', '--redefine-syms=' + str(mapping),
                            str(prefixed)], check=True)
            global_symbols = subprocess.check_output(
                [cross + 'nm', '-g', '--format=posix', str(prefixed)], text=True)
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
                [cross + 'nm', '-u', str(prefixed)], text=True).splitlines()
            symbols = subprocess.check_output([cross + 'readelf', '-sW', str(prefixed)], text=True)
            (out/'isolated-symbols.txt').write_text(symbols)
            linked['tls_symbols'] = [line.strip() for line in symbols.splitlines()
                                     if ' TLS ' in line]
            relocations = subprocess.check_output([cross + 'readelf', '-rW', str(prefixed)], text=True)
            (out/'isolated-relocations.txt').write_text(relocations)
            if args.emulated_tls:
                if linked['tls_symbols'] or re.search(r'R_\w*(?:TLS|TPREL|TPOFF|DTP|GOTTPOFF)', relocations):
                    raise SystemExit('native TLS remains in emulated BSD core')
                assembly = subprocess.check_output([cross + 'objdump', '-d', str(prefixed)], text=True)
                if args.arch == 'x86_64' and re.search(r'%[fg]s:', assembly):
                    raise SystemExit('BSD core still accesses FS/GS')
                linked['no_native_tls_checked'] = True
            elf = prefixed.read_bytes()
            expected_machine = {'x86_64': 62, 'aarch64': 183,
                                'riscv64': 243, 'loongarch64': 258}[args.arch]
            expected_class = 2
            if (elf[:4] != b'\x7fELF' or elf[4] != expected_class or elf[5] != 1 or
                    int.from_bytes(elf[18:20], 'little') != expected_machine):
                raise SystemExit('BSD object has the wrong target ELF class, endian or machine')
            linked['elf_target_checked'] = True
            linked['isolated_sha256'] = hashlib.sha256(elf).hexdigest()
            (out/'isolated-sections.txt').write_text(subprocess.check_output(
                [cross + 'readelf', '-SW', str(prefixed)], text=True))
    manifest = dict(arch=args.arch, net_profile=args.net_profile, emulated_tls=args.emulated_tls,
                    tls_lowering='explicit-accessors' if explicit_tls else 'compiler',
                    revision=pin['revision'], source_manifest_sha256=hashlib.sha256((port/'port-sources.toml').read_bytes()).hexdigest(), partial_link=linked, compiler=compiler_version, status='object-compilation-only-not-linked', sources=rows)
    manifest['port_headers'] = {str(path.relative_to(port)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in sorted(port.rglob('*.h'))}
    manifest['generated_inputs'] = {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
                                    for directory in (generated, staged_lib)
                                    for path in sorted(directory.iterdir()) if path.is_file()}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    for row in rows:
        print(f"{'COMPILED' if row['rc'] == 0 else 'FAILED'} {row['source']}")
    if git('status', '--porcelain', '--untracked-files=all'):
        raise SystemExit('probe modified upstream')
    raise SystemExit(1 if any(row['rc'] for row in rows) or (linked and linked['rc']) else 0)


if __name__ == '__main__':
    main()
