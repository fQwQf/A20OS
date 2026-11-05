#!/usr/bin/env python3
"""Link real GNU Classpath core JNI modules into a frozen Native JamVM.

Optional static modules retain JamVM class-loader ownership and normal JNI FFI.
No replacement libc or emulated successful JNI operations are provided.
This build alone does not verify Java application execution or GC.
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
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent.parent
CLASSPATH_SHA = 'f929297f8ae9b613a1a167e231566861893260651d913ad9b6c11933895fecc8'
ECJ_SHA = '9cddda75f4a1b4469e73f44e7b61a3e897d0f657df4797f9106ffe88c4eeade0'
MODULES = {
    'javaio': ('java-io', 'libjavaio_la_SOURCES'),
    'javanio': ('java-nio', 'libjavanio_la_SOURCES'),
    'javalang': ('java-lang', 'libjavalang_la_SOURCES'),
    'javalangreflect': ('java-lang', 'libjavalangreflect_la_SOURCES'),
    'javalangmanagement': ('java-lang', 'libjavalangmanagement_la_SOURCES'),
    'javautil': ('java-util', 'libjavautil_la_SOURCES'),
}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_list(path, variable):
    text = path.read_text().replace('\\\n', ' ')
    match = re.search(r'^' + re.escape(variable) + r'\s*=\s*(.*)$', text, re.M)
    if not match:
        raise ValueError('missing upstream source list: ' + variable)
    words = shlex.split(match[1])
    if any('$' in x for x in words):
        raise ValueError('unexpanded upstream source list: ' + variable)
    return [path.parent / x for x in words if x.endswith('.c')]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native-build', type=Path, required=True)
    p.add_argument('--archives', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--build-triplet', required=True)
    p.add_argument('--java', default='java')
    p.add_argument('--classlib', type=Path, help='include actual Java execution probe, using this verified class library')
    p.add_argument('--nm', help='target nm, defaults to the frozen Native build toolchain')
    args = p.parse_args()
    out, original = args.output.resolve(), args.native_build.resolve()
    if out.exists() and any(out.iterdir()):
        p.error('--output must be empty; retain failed evidence')
    base = json.loads((original / 'build.json').read_text())
    if not base['built'] or sha(original / 'jamvm.elf') != base['program_sha256']:
        p.error('completed, hash-matching Native build required')
    target = base.get('toolchain', dict(prefix='riscv64-unknown-elf-', host='riscv64-unknown-elf'))
    args.nm = args.nm or target['prefix'] + 'nm'
    out.mkdir(parents=True, exist_ok=True)
    report = dict(built=False, java_application_verified=False, gc_verified=False,
                  minecraft_verified=False, arch=base.get('arch', 'riscv64'), toolchain=target,
                  commands=[], native_base=base['program_sha256'])

    def save():
        (out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(cmd, cwd, phase, env=None):
        report['phase'] = phase
        report['commands'].append(dict(argv=list(map(str, cmd)), cwd=str(cwd), log=phase + '.log'))
        save()
        print(phase, flush=True)
        with (out / (phase + '.log')).open('w') as log:
            subprocess.run(cmd, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

    def hashes(directory):
        return {str(x.relative_to(directory)): sha(x) for x in sorted(directory.rglob('*')) if x.is_file()}

    save()
    try:
        frozen = out / 'inputs'
        frozen.mkdir()
        for name, digest in [('classpath-0.99.tar.gz', CLASSPATH_SHA), ('ecj-4.6.1.jar', ECJ_SHA)]:
            src = args.archives.resolve() / name
            if sha(src) != digest:
                raise ValueError('archive checksum mismatch: ' + name)
            shutil.copy2(src, frozen / name)
        shutil.copy2(Path(__file__), frozen / 'build-jamvm-jni.py')
        shutil.copy2(ROOT / 'user/ports/classpath/compiler-checks.patch', frozen / 'compiler-checks.patch')
        shutil.copytree(ROOT / 'user/ports/jamvm', frozen / 'port')
        if args.classlib:
            library = args.classlib.resolve()
            cm = json.loads((library / 'build.json').read_text())
            if not cm['built']:
                raise ValueError('completed class library required')
            for name in ['glibj.zip', 'classes.zip']:
                if sha(library / name) != cm['outputs'][name]:
                    raise ValueError('class library checksum mismatch: ' + name)
                shutil.copy2(library / name, frozen / name)
            shutil.copy2(ROOT / 'user/tests/test_jamvm_execution.c', frozen / 'test_jamvm_execution.c')
            for name in ['A20NativeWorkload.java', 'A20NativeGC.java']:
                shutil.copy2(ROOT / 'user/tests/java' / name, frozen / name)
        native = out / 'native'
        native.mkdir()
        for name in ['sysroot', 'objects', 'generated', 'deps', 'inputs']:
            shutil.copytree(original / name, native / name)
        # Header dependencies of the rebuilt OS adapter, not a second VM build.
        shutil.copytree(original / 'source/jamvm-2.0.0/src', native / 'source/jamvm-2.0.0/src')
        for name in ['build.json', 'jni-probe.elf']:
            shutil.copy2(original / name, native / name)
        wrapper = native / 'native-cc'
        wrapper.write_text((original / 'native-cc').read_text().replace(str(original), str(native)))
        wrapper.chmod(0o755)
        report['frozen_native_inputs'] = hashes(native)
        source = out / 'source'
        source.mkdir()
        with tarfile.open(frozen / 'classpath-0.99.tar.gz') as archive:
            archive.extractall(source, filter='data')
        cp = source / 'classpath-0.99'
        run(['patch', '-p1', '--fuzz=0', '-i', str(frozen / 'compiler-checks.patch')], cp, 'classpath-patch')
        config = out / 'classpath'
        config.mkdir()
        host_tools = out / 'host-tools'
        host_tools.mkdir()
        decoder = host_tools / 'uudecode'
        decoder.write_text('#!/usr/bin/env python3\n'
            'import base64, pathlib, sys\n'
            'lines=pathlib.Path(sys.argv[1]).read_text().splitlines()\n'
            'head=lines[0].split()\n'
            'if len(head)!=3 or head[0]!="begin-base64" or pathlib.Path(head[2]).name!=head[2] or lines[-1]!="====":\n'
            '    raise ValueError("expected base64 uudecode input with local output name")\n'
            'pathlib.Path(head[2]).write_bytes(base64.b64decode("".join(lines[1:-1]),validate=True))\n')
        decoder.chmod(0o755)
        env = os.environ.copy()
        env['PATH'] = str(host_tools) + os.pathsep + env['PATH']
        env.update(CC=str(wrapper), AR=target['prefix'] + 'ar', RANLIB=target['prefix'] + 'ranlib',
                   JAVAC=shlex.join([args.java, '-Xmx768m', '-jar', str(frozen / 'ecj-4.6.1.jar')]))
        command = [str(cp / 'configure'), '--build=' + args.build_triplet, '--host=' + target['host'],
                   '--prefix=/usr/lib/classpath', '--with-glibj=zip', '--enable-jni', '--enable-core-jni',
                   '--disable-shared', '--enable-static', '--disable-tools', '--disable-examples',
                   '--disable-gjdoc', '--disable-gtk-peer', '--disable-gconf-peer', '--disable-alsa',
                   '--disable-dssi', '--disable-gmp', '--disable-qt-peer', '--disable-gstreamer-peer',
                   '--disable-xmlj', '--disable-regen-headers', '--with-jar=no']
        run(command, config, 'classpath-configure', env)
        objects = out / 'objects'
        objects.mkdir()
        jni = cp / 'native/jni'
        macros = subprocess.check_output([str(wrapper), '-dM', '-E', '-x', 'c', '/dev/null'], text=True)
        (out / 'compiler-macros.log').write_text(macros)
        definitions = dict(re.findall(r'^#define (\w+) (.+)$', macros, re.M))
        if definitions.get('__SIZEOF_DOUBLE__') != '8' or definitions.get('__DBL_MANT_DIG__') != '53':
            raise ValueError('fdlibm requires IEEE binary64 double')
        order = definitions.get('__BYTE_ORDER__')
        if order not in ['__ORDER_LITTLE_ENDIAN__', '__ORDER_BIG_ENDIAN__']:
            raise ValueError('compiler must declare floating point byte order')
        endian = '-D__IEEE_' + ('LITTLE' if order == '__ORDER_LITTLE_ENDIAN__' else 'BIG') + '_ENDIAN'
        flags = [endian, '-O2', '-g', '-std=gnu99', '-fno-strict-aliasing', '-DHAVE_CONFIG_H',
                 '-I' + str(config / 'include'), '-I' + str(cp / 'include'),
                 '-I' + str(jni / 'classpath'), '-I' + str(jni / 'native-lib'),
                 '-I' + str(cp / 'native/fdlibm')]
        linked, table, headers = [], {}, set()
        counter = 0

        def compile_source(src, extra=(), vm=False):
            nonlocal counter
            obj = objects / (str(counter) + '-' + src.stem + '.o')
            counter += 1
            run([str(wrapper), *(flags if not vm else ['-O2', '-g', '-std=gnu99', '-fno-strict-aliasing']), *extra, '-c', str(src), '-o', str(obj)], out,
                'compile-' + obj.stem)
            linked.append(obj)
            return obj

        # Upstream explicitly supports lazy Pointer initialization without OnLoad.
        compile_source(jni / 'classpath/jcl.c', ['-DJCL_NO_JNIONLOAD'])
        for src in source_list(jni / 'native-lib/Makefile.am', 'libclasspathnative_la_SOURCES'):
            compile_source(src)
        for src in source_list(cp / 'native/fdlibm/Makefile.am', 'libfdlibm_la_SOURCES'):
            compile_source(src)
        for module, (directory, variable) in MODULES.items():
            symbols = []
            for src in source_list(jni / directory / 'Makefile.am', variable):
                header = cp / 'include' / (src.stem + '.h')
                if not header.is_file():
                    raise ValueError('missing actual JNI prototypes: ' + str(header))
                headers.add(header.name)
                obj = compile_source(src)
                listing = subprocess.check_output([args.nm, '-g', '--defined-only', str(obj)], text=True)
                for line in listing.splitlines():
                    fields = line.split()
                    if len(fields) == 3 and fields[1] == 'T' and (fields[2].startswith('Java_') or fields[2] in ['JNI_OnLoad', 'JNI_OnUnload']):
                        symbols.append(fields[2])
            if not symbols or len(symbols) != len(set(symbols)):
                raise ValueError('empty/duplicate module JNI symbols: ' + module)
            table[module] = sorted(symbols)
        if args.classlib:
            obj = compile_source(frozen / 'test_jamvm_execution.c',
                                 ['-I' + str(native / 'inputs/sdk/user/liba20rt')])
            table['a20execution'] = ['Java_A20NativeWorkload_cpu']
            listing = subprocess.check_output([args.nm, '-g', '--defined-only', str(obj)], text=True)
            if ' T Java_A20NativeWorkload_cpu' not in listing:
                raise ValueError('execution probe JNI symbol missing')
            classes = out / 'workload-classes'
            classes.mkdir()
            run([args.java, '-Xmx768m', '-jar', str(frozen / 'ecj-4.6.1.jar'),
                 '-source', '1.5', '-target', '1.5', '-bootclasspath', str(frozen / 'glibj.zip'),
                 '-d', str(classes), str(frozen / 'A20NativeWorkload.java'),
                 str(frozen / 'A20NativeGC.java')], out, 'java-workload-compile')
            with zipfile.ZipFile(out / 'workload.jar', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                for path in sorted(classes.rglob('*.class')):
                    data = path.read_bytes()
                    if data[:4] != b'\xca\xfe\xba\xbe' or int.from_bytes(data[6:8], 'big') != 49:
                        raise ValueError('expected real Java 5 class file')
                    archive.write(path, str(path.relative_to(classes)))
            report['java_workload_sha256'] = sha(out / 'workload.jar')
            report['gc_workload_built'] = True
        registry = out / 'registry.c'
        text = '#include "static-jni.h"\n#include <jni.h>\n'
        text += ''.join('#include <' + header + '>\n' for header in sorted(headers))
        if args.classlib:
            text += 'extern jint JNICALL Java_A20NativeWorkload_cpu(JNIEnv *, jclass);\n'
        for module, symbols in table.items():
            text += 'static const A20JNISymbol symbols_' + module + '[] = {\n'
            text += ''.join('    {"' + s + '", (void (*)(void))' + s + '},\n' for s in symbols) + '};\n'
        text += 'const A20JNIModule a20_jni_modules[] = {\n'
        for module, symbols in table.items():
            text += '    {"/usr/lib/classpath/lib/classpath/lib' + module + '.so", symbols_' + module + ', ' + str(len(symbols)) + '},\n'
        text += '};\nconst size_t a20_jni_module_count = sizeof(a20_jni_modules) / sizeof(a20_jni_modules[0]);\n'
        registry.write_text(text)
        port = frozen / 'port'
        compile_source(registry, ['-I' + str(port)])
        compile_source(port / 'static-jni.c', ['-I' + str(port)])
        jam = native / 'source/jamvm-2.0.0/src'
        compile_source(port / 'os.c', ['-DA20_STATIC_JNI', '-I' + str(native / 'generated'),
                    '-I' + str(jam), '-I' + str(native / 'inputs/sdk/user/liba20rt')], vm=True)
        vm_objects = sorted((native / 'objects').glob('*.o'), key=lambda x: int(x.name.split('-')[0]))
        if len(vm_objects) != base['translation_units'] or sum(x.name.endswith('-os.o') for x in vm_objects) != 1:
            raise ValueError('unexpected frozen VM object layout')
        vm_objects = [x for x in vm_objects if not x.name.endswith('-os.o')]
        program = out / 'jamvm.elf'
        run([str(wrapper), *map(str, vm_objects), *map(str, linked), '-L' + str(native / 'deps/lib'),
             '-lffi', '-lz', '-o', str(program)], out, 'jamvm-jni-link')
        shutil.copy2(native / 'jni-probe.elf', out / 'jni-probe.elf')
        report.update(built=True, phase='built', program_sha256=sha(program),
                      jni_probe_sha256=sha(out / 'jni-probe.elf'), modules=table,
                      native_classpath_jni_built=True, module_lifetime='executable-pinned',
                      configuration=hashes(config / 'include'), frozen_inputs=hashes(frozen),
                      outputs=hashes(objects), registry_sha256=sha(registry))
        print('BUILT actual Classpath static JNI ' + report['program_sha256'], flush=True)
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
