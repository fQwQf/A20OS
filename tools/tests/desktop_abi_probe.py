#!/usr/bin/env python3
"""Run Linux ABI regressions inside an A20OS Python-equipped desktop world.

Also runnable on Linux as a reference. No packages or persistent settings are
changed; temporary scripts and the private SysV segment are removed on exit.
"""
import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def exec_scripts():
    with tempfile.TemporaryDirectory(prefix='a20-exec-script-') as directory:
        root = Path(directory)
        for interpreter in ('/bin/sh', '/usr/bin/env sh'):
            script = root / 'trigger.sh'
            script.write_text('#!' + interpreter + '\nprintf "%s\\n" "$0"\n')
            script.chmod(0o755)
            for spelling in ('./trigger.sh', str(script)):
                result = subprocess.run([spelling], cwd=root, capture_output=True,
                                        text=True, timeout=20, check=True)
                assert result.stdout == spelling + '\n', repr(result.stdout)


def shm_after_rmid():
    libc = ctypes.CDLL(None, use_errno=True)
    libc.shmat.restype = ctypes.c_void_p
    libc.shmdt.argtypes = [ctypes.c_void_p]
    failed = ctypes.c_void_p(-1).value
    segment = libc.shmget(0, 4096, 0o1600)  # IPC_PRIVATE, IPC_CREAT | 0600
    assert segment >= 0, ctypes.get_errno()
    first = libc.shmat(segment, None, 0)
    assert first != failed, ctypes.get_errno()
    try:
        ctypes.memmove(first, b'parent\0', 7)
        assert libc.shmctl(segment, 0, None) == 0  # IPC_RMID
        child = '''import ctypes,sys
c=ctypes.CDLL(None,use_errno=True); c.shmat.restype=ctypes.c_void_p
c.shmdt.argtypes=[ctypes.c_void_p]
p=c.shmat(int(sys.argv[1]),None,0)
assert p!=ctypes.c_void_p(-1).value, ctypes.get_errno()
try:
    assert ctypes.string_at(p)==b'parent'
    ctypes.memmove(p,b'plugin\\0',7)
finally:
    assert c.shmdt(p)==0
'''
        subprocess.run([sys.executable, '-c', child, str(segment)],
                       check=True, timeout=20)
        assert ctypes.string_at(first) == b'plugin'
    finally:
        libc.shmctl(segment, 0, None)
        assert libc.shmdt(first) == 0
    assert libc.shmat(segment, None, 0) == failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', choices=('exec-script', 'shm-rmid'),
                        action='append')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    checks = {'exec-script': exec_scripts, 'shm-rmid': shm_after_rmid}
    results = {}
    for name in args.check or checks:
        try:
            checks[name]()
            results[name] = {'status': 'PASS'}
        except Exception as error:
            results[name] = {'status': 'FAIL', 'error': repr(error)}
    report = json.dumps(results, indent=2)
    print(report)
    if args.output:
        args.output.write_text(report + '\n')
    return int(any(r['status'] != 'PASS' for r in results.values()))


if __name__ == '__main__':
    sys.exit(main())
