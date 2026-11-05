#!/usr/bin/env python3
"""Build unmodified Lua and SQLite sources against an isolated Native sysroot.

Sources are supplied locally. Output records commands, source hashes, libc hash
and configuration; successful compilation does not assert runtime acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent.parent

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=['riscv64', 'aarch64'], default='riscv64')
    parser.add_argument('--lua', type=Path, required=True, help='Lua source root with src/')
    parser.add_argument('--sqlite', type=Path, required=True, help='SQLite amalgamation directory')
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc')
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists() and any(out.iterdir()): parser.error('output must be empty; preserve failed evidence')
    out.mkdir(parents=True, exist_ok=True)
    report = dict(built=False, runtime_verified=False, arch=args.arch, commands=[], source_hashes={})
    def save(): (out/'build.json').write_text(json.dumps(report, indent=2)+'\n')
    root = args.sysroot.resolve()
    libc = root/'lib/libc.a'
    report['libc_sha256'] = sha(libc)
    report['configuration'] = ['LUA_USE_POSIX', 'SQLITE_OS_UNIX=1', 'SQLITE_THREADSAFE=1', 'SQLITE_OMIT_LOAD_EXTENSION=1', 'HAVE_PREAD=1', 'HAVE_PWRITE=1']
    cc = args.cc or {'riscv64':'riscv64-unknown-elf-gcc','aarch64':'aarch64-linux-gnu-gcc'}[args.arch]
    common = [cc, '-O2', '-fno-pie', '-D_GNU_SOURCE', '-D__a20__', '-U__linux__', '-U__linux', '-Ulinux', '-isystem', str(root/'include')]
    if args.arch == 'riscv64':
        common += ['-march=rv64gc','-mabi=lp64d','-mcmodel=medany']
        common += ['-D__INT_FAST8_TYPE__=signed char','-D__INT_FAST16_TYPE__=long','-D__INT_FAST32_TYPE__=long','-D__UINT_FAST8_TYPE__=unsigned char','-D__UINT_FAST16_TYPE__=unsigned long','-D__UINT_FAST32_TYPE__=unsigned long']
    else: common += ['-mno-outline-atomics']
    def run(command, label):
        report['commands'].append(command); save()
        with (out/(label+'.log')).open('w') as log:
            subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    def compile(source, label, flags):
        source=source.resolve(); report['source_hashes'][str(source)]=sha(source)
        target=out/(label+'.o')
        run(common+flags+['-c',str(source),'-o',str(target)],label)
        return str(target)
    tag = 'rv' if args.arch=='riscv64' else 'aarch64'
    def link(name, objects):
        target=out/('mlibc-'+name+'-'+tag)
        run(common+['-static','-nostdlib','-no-pie','-Wl,-z,max-page-size=4096','-Wl,-s','-T',str(ROOT/'user/mlibc/a20-mlibc.ld'),str(root/'lib/crt1.o'),str(root/'lib/a20_thread_entry.o')]+objects+['-L'+str(root/'lib'),'-lc','-lpthread','-lm','-lrt','-ldl','-lgcc','-o',str(target)],name+'-link')
        return target
    try:
        lua_objects=[]
        for source in sorted((args.lua/'src').glob('*.c')):
            if source.name in ['lua.c','luac.c']: continue
            lua_objects.append(compile(source,'lua-'+source.stem,['-DLUA_USE_POSIX']))
        main_object=compile(args.lua/'src/lua.c','lua-main',['-DLUA_USE_POSIX'])
        programs=[link('lua',lua_objects+[main_object])]
        flags=['-DSQLITE_OS_UNIX=1','-DSQLITE_THREADSAFE=1','-DSQLITE_OMIT_LOAD_EXTENSION=1','-DHAVE_PREAD=1','-DHAVE_PWRITE=1','-I'+str(args.sqlite.resolve())]
        sqlite_object=compile(args.sqlite/'sqlite3.c','sqlite-core',flags)
        cli=compile(args.sqlite/'shell.c','sqlite-cli',flags)
        programs.append(link('sqlite',[sqlite_object,cli]))
        test=compile(ROOT/'user/tests/test_mlibc_sqlite.c','sqlite-test',flags)
        programs.append(link('sqlite-test',[sqlite_object,test]))
        if report['libc_sha256']!=sha(libc): raise RuntimeError('sysroot changed during the build')
        for path,digest in report['source_hashes'].items():
            if sha(Path(path))!=digest: raise RuntimeError('source changed during the build')
        report['programs']={p.name:sha(p) for p in programs}
        report['built']=True
    finally: save()
    print('BUILT Native Lua and SQLite; runtime verification is separate')

if __name__=='__main__': main()
