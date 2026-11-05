#!/usr/bin/env python3
"""Build real GNU Classpath 0.99 and JamVM VM classes for the Native port.

Only Java bytecode/resources are built here; native Classpath JNI libraries,
Java execution, GC and Minecraft require further runtime implementation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent.parent
PINNED = {
    'classpath-0.99.tar.gz': 'f929297f8ae9b613a1a167e231566861893260651d913ad9b6c11933895fecc8',
    'jamvm-2.0.0.tar.gz': '76428e96df0ae9dd964c7a7c74c1e9a837e2f312c39e9a357fa8178f7eff80da',
    'ecj-4.6.1.jar': '9cddda75f4a1b4469e73f44e7b61a3e897d0f657df4797f9106ffe88c4eeade0',
}
UUD = '''import base64, pathlib, sys
lines=pathlib.Path(sys.argv[1]).read_text().splitlines()
head=lines[0].split()
if len(head)!=3 or head[0]!='begin-base64' or pathlib.Path(head[2]).name!=head[2] or lines[-1]!='====':
    raise ValueError('expected base64 uudecode input with local output name')
pathlib.Path(head[2]).write_bytes(base64.b64decode(''.join(lines[1:-1]),validate=True))
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def classes(path):
    count = 0
    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if name.endswith('.class'):
                data = archive.read(name)
                if data[:4] != b'\xca\xfe\xba\xbe' or int.from_bytes(data[6:8], 'big') != 49:
                    raise ValueError('expected genuine Java 5 bytecode: ' + name)
                count += 1
    if not count:
        raise ValueError('empty class archive')
    return count


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archives', type=Path, required=True)
    p.add_argument('--native-build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--build-triplet', required=True, help='real host C compiler triplet, e.g. x86_64-pc-linux-gnu')
    p.add_argument('--java', default='java')
    args = p.parse_args()
    out, native = args.output.resolve(), args.native_build.resolve()
    if out.exists() and any(out.iterdir()):
        p.error('--output must be empty; retain failed evidence')
    manifest = json.loads((native / 'build.json').read_text())
    if not manifest['built'] or sha(native / 'jamvm.elf') != manifest['program_sha256']:
        p.error('completed Native build required')
    out.mkdir(parents=True, exist_ok=True)
    report = dict(built=False, native_classpath_jni_built=False,
                  java_application_verified=False, commands=[])

    def save():
        (out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(cmd, cwd, phase, env=None):
        report['phase'] = phase
        report['commands'].append(dict(argv=list(map(str, cmd)), cwd=str(cwd), log=phase + '.log'))
        save()
        print(phase, flush=True)
        with (out / (phase + '.log')).open('w') as log:
            subprocess.run(cmd, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

    save()
    try:
        frozen = out / 'inputs'
        frozen.mkdir()
        report['archives'] = {}
        for name, digest in PINNED.items():
            src = args.archives.resolve() / name
            if sha(src) != digest:
                raise ValueError('checksum mismatch: ' + name)
            shutil.copyfile(src, frozen / name)
            report['archives'][name] = digest
        shutil.copyfile(ROOT / 'user/ports/classpath/compiler-checks.patch', frozen / 'compiler-checks.patch')
        shutil.copyfile(Path(__file__), frozen / 'build-jamvm-classlib.py')
        shutil.copyfile(native / 'build.json', frozen / 'native-build.json')
        source = out / 'source'
        source.mkdir()
        for name in ['classpath-0.99.tar.gz', 'jamvm-2.0.0.tar.gz']:
            with tarfile.open(frozen / name) as archive:
                archive.extractall(source, filter='data')
        cp = source / 'classpath-0.99'
        run(['patch', '-p1', '--fuzz=0', '-i', str(frozen / 'compiler-checks.patch')], cp, 'compiler-checks-patch')
        host_tools = out / 'host-tools'
        host_tools.mkdir()
        decoder = host_tools / 'uudecode'
        decoder.write_text('#!' + sys.executable + '\n' + UUD)
        decoder.chmod(0o755)
        env = os.environ.copy()
        env.update(PATH=str(host_tools) + os.pathsep + env['PATH'], CC=str(native / 'native-cc'),
                   JAVAC=shlex.join([args.java, '-Xmx768m', '-jar', str(frozen / 'ecj-4.6.1.jar')]))
        build = out / 'classpath'
        build.mkdir()
        configure = [str(cp / 'configure'), '--build=' + args.build_triplet, '--host=riscv64-unknown-elf',
                     '--prefix=/usr/lib/classpath', '--with-glibj=zip', '--disable-jni', '--enable-core-jni',
                     '--disable-tools', '--disable-examples', '--disable-gjdoc', '--disable-gtk-peer',
                     '--disable-gconf-peer', '--disable-alsa', '--disable-dssi', '--disable-gmp',
                     '--disable-qt-peer', '--disable-gstreamer-peer', '--disable-xmlj',
                     '--disable-regen-headers', '--with-jar=no']
        run(configure, build, 'classpath-configure', env)
        run(['make', '-j4', '-C', 'lib'], build, 'classpath-classes', env)
        shutil.copyfile(build / 'lib/glibj.zip', out / 'glibj.zip')
        vm = source / 'jamvm-2.0.0/src/classlib/gnuclasspath/lib'
        vm_out = out / 'vm-classes'
        vm_out.mkdir()
        java_sources = sorted(vm.rglob('*.java'))
        argfile = out / 'vm-sources.args'
        argfile.write_text('\n'.join('"' + str(x) + '"' for x in java_sources) + '\n')
        run([args.java, '-Xmx768m', '-jar', str(frozen / 'ecj-4.6.1.jar'), '-source', '1.5', '-target', '1.5',
             '-bootclasspath', str(out / 'glibj.zip'), '-d', str(vm_out), '@' + str(argfile)], out, 'jamvm-classes')
        with zipfile.ZipFile(out / 'classes.zip', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for file in sorted(vm_out.rglob('*.class')):
                archive.write(file, str(file.relative_to(vm_out)))
        report['class_counts'] = {name: classes(out / name) for name in ['glibj.zip', 'classes.zip']}
        report['outputs'] = {name: sha(out / name) for name in ['glibj.zip', 'classes.zip']}
        report['frozen_inputs'] = {str(x.relative_to(out)): sha(x) for x in sorted(frozen.iterdir()) if x.is_file()}
        report.update(built=True, phase='built')
        print('BUILT real GNU Classpath and JamVM classes ' + str(report['class_counts']), flush=True)
    except Exception as e:
        report['failure'] = str(e)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
