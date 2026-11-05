#!/usr/bin/env python3
"""Build upstream JamVM 2.0.0 on a Native full-libc port.

This is an optional interpreter/JNI build, not Java or Minecraft acceptance.
Requires an existing A20 mlibc sysroot and three checksum-pinned source archives.
No downloads, host libc objects, Linux syscall stubs, or default rootfs changes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parent.parent.parent
TARGETS = {
    'riscv64': dict(prefix='riscv64-unknown-elf-', host='riscv64-unknown-elf',
                    flags=['-march=rv64gc', '-mabi=lp64d', '-mcmodel=medany']),
    'aarch64': dict(prefix='aarch64-linux-gnu-', host='aarch64-unknown-elf',
                    flags=['-fno-pie', '-mno-outline-atomics', '-U__linux__', '-U__linux', '-Ulinux']),
}
ARCHIVES = {
    'jamvm-2.0.0': '76428e96df0ae9dd964c7a7c74c1e9a837e2f312c39e9a357fa8178f7eff80da',
    'zlib-1.3.1': '9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23',
    'libffi-3.4.6': 'b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources(makefile, variable):
    logical = makefile.read_text().replace('\\\n', ' ')
    match = re.search(r'^' + variable + r'\s*=\s*(.*)$', logical, re.M)
    if not match:
        raise RuntimeError(f'missing upstream source list {variable}')
    return [x for x in shlex.split(match[1]) if x.endswith('.c')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archives', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--arch', choices=list(TARGETS), default='riscv64')
    parser.add_argument('--cc', help='target C compiler, defaults to the selected architecture')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    target = TARGETS[args.arch]
    args.cc = args.cc or target['prefix'] + 'gcc'
    output = args.output.resolve()
    sysroot = args.sysroot.resolve()
    archives = args.archives.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('--output must be empty; preserve failed builds and evidence')
    if not (sysroot / 'lib/libc.a').is_file():
        parser.error('build the Native full-mlibc sysroot first')
    output.mkdir(parents=True, exist_ok=True)
    report = dict(built=False, java_bytecode_executed=False, minecraft=False,
                  arch=args.arch, abi='a20-native', toolchain=target, commands=[])

    def save():
        (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(command, cwd, name, env=None):
        report['phase'] = name
        report['commands'].append(dict(argv=list(map(str, command)), cwd=str(cwd), log=name + '.log'))
        save()
        print(name, flush=True)
        with (output / (name + '.log')).open('w') as log:
            subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

    save()
    try:
        report['archives'] = {}
        for name, expected in ARCHIVES.items():
            archive = archives / (name + '.tar.gz')
            digest = sha(archive)
            if digest != expected:
                raise RuntimeError(f'checksum mismatch: {archive}')
            report['archives'][archive.name] = digest
        inputs_dir = output / 'inputs'
        inputs_dir.mkdir()
        shutil.copytree(sysroot, output / 'sysroot')
        sysroot = output / 'sysroot'
        shutil.copytree(ROOT / 'user/ports/jamvm', inputs_dir / 'port')
        shutil.copytree(ROOT / 'user/liba20rt', inputs_dir / 'sdk/user/liba20rt')
        shutil.copytree(ROOT / 'kernel/include/uapi/a20', inputs_dir / 'sdk/kernel/include/uapi/a20')
        shutil.copyfile(ROOT / 'user/mlibc/a20-mlibc.ld', inputs_dir / 'a20-mlibc.ld')
        shutil.copyfile(ROOT / 'user/tests/test_jamvm_jni.c', inputs_dir / 'test_jamvm_jni.c')
        shutil.copyfile(Path(__file__), inputs_dir / 'build-jamvm-native.py')
        port = inputs_dir / 'port'
        report['sysroot'] = {str(p.relative_to(sysroot)): sha(p) for p in sorted(sysroot.rglob('*')) if p.is_file()}
        report['port'] = {str(p.relative_to(inputs_dir)): sha(p) for p in sorted(port.glob('*')) if p.is_file()}
        report['jni_probe_source'] = sha(inputs_dir / 'test_jamvm_jni.c')
        report['build_driver'] = sha(Path(__file__))
        report['linker_script'] = sha(inputs_dir / 'a20-mlibc.ld')
        report['compiler'] = subprocess.check_output([args.cc, '--version'], text=True).splitlines()[0]
        source = output / 'source'
        source.mkdir()
        for name in ARCHIVES:
            with tarfile.open(archives / (name + '.tar.gz')) as archive:
                archive.extractall(source, filter='data')
        wrapper = output / 'native-cc'
        cflags = [*target['flags'], '-fno-stack-protector',
                  '-D_GNU_SOURCE', '-D__a20__', '-D__INT_FAST8_TYPE__=signed char',
                  '-D__INT_FAST16_TYPE__=long', '-D__INT_FAST32_TYPE__=long',
                  '-D__UINT_FAST8_TYPE__=unsigned char', '-D__UINT_FAST16_TYPE__=unsigned long',
                  '-D__UINT_FAST32_TYPE__=unsigned long', '-isystem', str(sysroot / 'include')]
        link_head = ['-static', '-nostdlib', '-T', str(inputs_dir / 'a20-mlibc.ld'),
                     str(sysroot / 'lib/crt1.o'), str(sysroot / 'lib/a20_thread_entry.o')]
        if args.arch == 'aarch64':
            link_head += ['-no-pie', '-Wl,-z,max-page-size=4096']
        link_tail = ['-L' + str(sysroot / 'lib'), '-Wl,--start-group', '-lc', '-lpthread', '-lm', '-lrt', '-ldl', '-lgcc', '-Wl,--end-group']
        wrapper.write_text('#!' + sys.executable + '\nimport os, sys\n'
                           + 'args=sys.argv[1:]\n'
                           + 'link=not any(x in args for x in ["-c", "-E", "-S", "--version", "-v", "-dumpmachine"])\n'
                           + 'cmd=' + repr([args.cc, *cflags]) + '\n'
                           + 'cmd += (' + repr(link_head) + ' if link else []) + args + (' + repr(link_tail) + ' if link else [])\n'
                           + 'os.execvp(cmd[0], cmd)\n')
        wrapper.chmod(0o755)
        env = os.environ.copy()
        env.update(CC=str(wrapper), AR=target['prefix'] + 'ar', RANLIB=target['prefix'] + 'ranlib',
                   STRIP=target['prefix'] + 'strip', CFLAGS='-O2 -fPIC', LDFLAGS='', LIBS='')
        deps = output / 'deps'
        run(['./configure', '--static', '--prefix=' + str(deps)], source / 'zlib-1.3.1', 'zlib-configure', env)
        run(['make', '-j' + str(args.jobs)], source / 'zlib-1.3.1', 'zlib-build', env)
        run(['make', 'install'], source / 'zlib-1.3.1', 'zlib-install', env)
        ffi = output / 'ffi'
        ffi.mkdir()
        run([str(source / 'libffi-3.4.6/configure'), '--host=' + target['host'], '--disable-shared',
             '--enable-static', '--disable-docs', '--disable-multi-os-directory', '--prefix=' + str(deps)], ffi, 'ffi-configure', env)
        run(['make', '-j' + str(args.jobs)], ffi, 'ffi-build', env)
        run(['make', 'install'], ffi, 'ffi-install', env)
        run(['patch', '-p1', '--fuzz=0', '-i', str(port / 'native.patch')],
            source / 'jamvm-2.0.0', 'jamvm-native-patch')
        run(['patch', '-p1', '--fuzz=0', '-i', str(port / 'gc.patch')],
            source / 'jamvm-2.0.0/src', 'jamvm-native-gc-patch')
        run(['patch', '-p1', '--fuzz=0', '-i', str(port / 'jni.patch')],
            source / 'jamvm-2.0.0/src', 'jamvm-native-jni-patch')
        jam = source / 'jamvm-2.0.0/src'
        generated = output / 'generated'
        generated.mkdir()
        for name in ['arch.h', 'config.h']:
            shutil.copyfile(port / name, generated / name)
        for name in ['classlib.h', 'classlib-defs.h', 'classlib-symbol.h', 'classlib-excep.h']:
            shutil.copyfile(jam / 'classlib/gnuclasspath' / name, generated / name)
        compile_flags = ['-O2', '-g', '-std=gnu99', '-fgnu89-inline', '-fno-strict-aliasing',
                         '-I' + str(generated), '-I' + str(jam), '-I' + str(jam / 'interp'),
                         '-I' + str(jam / 'interp/engine'), '-I' + str(jam / 'classlib/gnuclasspath'),
                         '-I' + str(deps / 'include'), '-I' + str(inputs_dir / 'sdk/user/liba20rt')]
        inputs = [jam / name for name in sources(jam / 'Makefile.am', 'libcore_la_SOURCES')]
        inputs += [jam / 'interp' / name for name in sources(jam / 'interp/Makefile.am', 'libinterp_la_SOURCES')]
        inputs += [jam / 'interp/engine' / name for name in sources(jam / 'interp/engine/Makefile.am', 'libengine_la_SOURCES')]
        inputs += [jam / 'classlib/gnuclasspath' / name for name in sources(jam / 'classlib/gnuclasspath/Makefile.am', 'libclasslib_la_SOURCES')]
        inputs += [port / 'os.c', jam / 'jam.c']
        objects = output / 'objects'
        objects.mkdir()
        built = []
        for index, path in enumerate(inputs):
            obj = objects / (str(index) + '-' + path.stem + '.o')
            run([str(wrapper), *compile_flags, '-c', str(path), '-o', str(obj)], ROOT, f'compile-{index:02d}-{path.stem}')
            built.append(str(obj))
        probe = output / 'jni-probe.elf'
        adapter = built[inputs.index(jam / 'dll_ffi.c')]
        run([str(wrapper), *compile_flags, str(inputs_dir / 'test_jamvm_jni.c'),
             adapter, '-L' + str(deps / 'lib'), '-lffi', '-o', str(probe)], ROOT, 'jni-probe-link')
        program = output / 'jamvm.elf'
        run([str(wrapper), *built, '-L' + str(deps / 'lib'), '-lffi', '-lz', '-o', str(program)], ROOT, 'jamvm-link')
        report['frozen_inputs'] = {str(p.relative_to(output)): sha(p) for p in sorted(inputs_dir.rglob('*')) if p.is_file()}
        report.update(built=True, program_sha256=sha(program), jni_probe_sha256=sha(probe), translation_units=len(inputs),
                      phase='built', dependency_libraries={str(p.relative_to(deps)): sha(p) for p in sorted(deps.rglob('*.a'))})
        print('BUILT Native JamVM ' + report['program_sha256'], flush=True)
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
