#!/usr/bin/env python3
"""Rebuild the complete Native JamVM core with cooperative GC safepoints.

Reuses immutable, hash-verified libc/dependency/JNI objects from a completed JNI
build; freezes all changed inputs and records the exact reused files. It does not
copy large unchanged libc archives. Keep the base build or its verified archive
for reproducibility. Real GC acceptance is a separate runtime gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def files(directory):
    return {str(p.relative_to(directory)): sha(p) for p in sorted(directory.rglob('*')) if p.is_file()}


def sources(path, variable):
    text = path.read_text().replace('\\\n', ' ')
    match = re.search(r'^' + re.escape(variable) + r'\s*=\s*(.*)$', text, re.M)
    if not match:
        raise ValueError('missing upstream source list: ' + variable)
    return [path.parent / x for x in shlex.split(match[1]) if x.endswith('.c')]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--jni-build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--java', default='java')
    args = p.parse_args()
    base, out = args.jni_build.resolve(), args.output.resolve()
    if out.is_relative_to(base):
        p.error('--output must be outside the frozen base build')
    if out.exists() and any(out.iterdir()):
        p.error('--output must be empty; preserve failed evidence')
    bm = json.loads((base / 'build.json').read_text())
    if not bm['built'] or not bm.get('native_classpath_jni_built') or sha(base / 'jamvm.elf') != bm['program_sha256']:
        p.error('completed, hash-matching Native JNI build required')
    native = base / 'native'
    reused = {'native/' + name: value for name, value in bm['frozen_native_inputs'].items()}
    reused.update({'objects/' + name: value for name, value in bm['outputs'].items()})
    reused['build.json'] = sha(base / 'build.json')
    for name, value in reused.items():
        if sha(base / name) != value:
            p.error('frozen base checksum mismatch: ' + name)
    out.mkdir(parents=True, exist_ok=True)
    report = dict(built=False, gc_workload_built=False, gc_verified=False,
                  java_application_verified=False, minecraft_verified=False,
                  arch=bm.get('arch', 'riscv64'),
                  toolchain=bm.get('toolchain') or dict(prefix='riscv64-unknown-elf-', host='riscv64-unknown-elf'),
                  base_build=str(base), base_program_sha256=bm['program_sha256'],
                  reused_inputs=reused, commands=[])

    def save():
        (out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(cmd, phase, cwd=out):
        report['phase'] = phase
        report['commands'].append(dict(argv=list(map(str, cmd)), cwd=str(cwd), log=phase + '.log'))
        save()
        print(phase, flush=True)
        with (out / (phase + '.log')).open('w') as log:
            subprocess.run(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)

    save()
    try:
        frozen = out / 'inputs'
        frozen.mkdir()
        shutil.copytree(ROOT / 'user/ports/jamvm', frozen / 'port')
        shutil.copytree(native / 'inputs/sdk', frozen / 'sdk')
        shutil.copy2(Path(__file__), frozen / 'build-jamvm-gc.py')
        shutil.copy2(ROOT / 'tools/build-jamvm-native.py', frozen / 'build-jamvm-native.py')
        for name in ['A20NativeWorkload.java', 'A20NativeGC.java']:
            shutil.copy2(ROOT / 'user/tests/java' / name, frozen / name)
        for name in ['glibj.zip', 'classes.zip', 'ecj-4.6.1.jar']:
            shutil.copy2(base / 'inputs' / name, frozen / name)
            if sha(frozen / name) != bm['frozen_inputs'][name]:
                raise ValueError('class/compiler archive checksum mismatch: ' + name)
        shutil.copy2(ROOT / 'user/tests/test_jamvm_jni.c', frozen / 'test_jamvm_jni.c')
        source = out / 'source'
        shutil.copytree(native / 'source/jamvm-2.0.0', source)
        patch = ['patch', '-p1', '--fuzz=0', '-i', str(frozen / 'port/gc.patch')]
        if 'void nativeThreadSafepoint(' in (source / 'src/thread.c').read_text():
            run([*patch, '--reverse', '--dry-run'], 'gc-existing-patch-check', source / 'src')
        else:
            run(patch, 'gc-patch', source / 'src')
        patch = ['patch', '-p1', '--fuzz=0', '-i', str(frozen / 'port/jni.patch')]
        if 'return &ffi_type_sint32;' in (source / 'src/dll_ffi.c').read_text():
            run([*patch, '--reverse', '--dry-run'], 'jni-existing-patch-check', source / 'src')
        else:
            run(patch, 'jni-patch', source / 'src')
        generated = out / 'generated'
        shutil.copytree(native / 'generated', generated)
        port = frozen / 'port'
        if sha(generated / 'config.h') != sha(port / 'config.h'):
            raise ValueError('base and current VM configuration differ: config.h')
        # The entire core is rebuilt with the current architecture adapter.
        # JNI objects use the same target compiler/dependencies and public JNI ABI.
        shutil.copy2(port / 'arch.h', generated / 'arch.h')
        wrapper = out / 'native-cc'
        shutil.copy2(native / 'native-cc', wrapper)
        wrapper.chmod(0o755)
        jam = source / 'src'
        flags = ['-O2', '-g', '-std=gnu99', '-fgnu89-inline', '-fno-strict-aliasing',
                 '-I' + str(generated), '-I' + str(jam), '-I' + str(jam / 'interp'),
                 '-I' + str(jam / 'interp/engine'), '-I' + str(jam / 'classlib/gnuclasspath'),
                 '-I' + str(native / 'deps/include'), '-I' + str(frozen / 'sdk/user/liba20rt')]
        inputs = sources(jam / 'Makefile.am', 'libcore_la_SOURCES')
        inputs += sources(jam / 'interp/Makefile.am', 'libinterp_la_SOURCES')
        inputs += sources(jam / 'interp/engine/Makefile.am', 'libengine_la_SOURCES')
        inputs += sources(jam / 'classlib/gnuclasspath/Makefile.am', 'libclasslib_la_SOURCES')
        inputs += [port / 'os.c', jam / 'jam.c']
        if len(inputs) != 46:
            raise ValueError('unexpected upstream core translation units')
        objects = out / 'objects'
        objects.mkdir()
        built = []
        for index, path in enumerate(inputs):
            obj = objects / (str(index) + '-' + path.stem + '.o')
            extra = ['-DA20_STATIC_JNI'] if path == port / 'os.c' else []
            run([str(wrapper), *flags, *extra, '-c', str(path), '-o', str(obj)], 'compile-' + obj.stem)
            built.append(obj)
        # The upstream JNI modules/registry do not access VM Thread internals.
        # Their symbols, exact bytes and owning modules remain the frozen base.
        jni = [base / 'objects' / name for name in bm['outputs'] if not name.endswith('-os.o')]
        if len(jni) != 80:
            raise ValueError('unexpected frozen JNI object layout')
        program = out / 'jamvm.elf'
        run([str(wrapper), *map(str, built), *map(str, jni), '-L' + str(native / 'deps/lib'),
             '-lffi', '-lz', '-o', str(program)], 'jamvm-link')
        run([str(wrapper), *flags, str(frozen / 'test_jamvm_jni.c'),
             str(built[inputs.index(jam / 'dll_ffi.c')]), '-L' + str(native / 'deps/lib'),
             '-lffi', '-o', str(out / 'jni-probe.elf')], 'jni-probe-link')
        classes = out / 'classes'
        classes.mkdir()
        run([args.java, '-Xmx768m', '-jar', str(frozen / 'ecj-4.6.1.jar'),
             '-source', '1.5', '-target', '1.5', '-bootclasspath', str(frozen / 'glibj.zip'),
             '-d', str(classes), str(frozen / 'A20NativeWorkload.java'),
             str(frozen / 'A20NativeGC.java')], 'java-compile')
        with zipfile.ZipFile(out / 'workload.jar', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(classes.rglob('*.class')):
                data = path.read_bytes()
                if data[:4] != b'\xca\xfe\xba\xbe' or int.from_bytes(data[6:8], 'big') != 49:
                    raise ValueError('expected actual Java 5 bytecode')
                archive.write(path, str(path.relative_to(classes)))
        for name, value in reused.items():
            if sha(base / name) != value:
                raise ValueError('frozen base changed while building: ' + name)
        report.update(built=True, phase='built', program_sha256=sha(program),
                      jni_probe_sha256=sha(out / 'jni-probe.elf'),
                      java_workload_sha256=sha(out / 'workload.jar'), gc_workload_built=True,
                      frozen_inputs=files(frozen), patched_source=files(source),
                      object_sha256=files(objects), modules=bm['modules'])
        print('BUILT Native safepoints ' + report['program_sha256'], flush=True)
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
