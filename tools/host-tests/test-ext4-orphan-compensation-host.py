#!/usr/bin/env python3
"""One real ext4 orphan compensation regression; no guest or full-suite build.

Uses the production Rust cache, orphan and extent controllers with the shared
C ext4 backend. A transient pair of write failures must recover; exhausting
both restoration attempts must quarantine and refuse a fresh-process mount.
Requires Linux/glibc, cc, rustc and e2fsprogs. Set EVIDENCE_DIR for the outputs.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, log):
    result = subprocess.run([str(v) for v in argv], cwd=ROOT,
                            capture_output=True, text=True, timeout=120,
                            env=dict(os.environ, LC_ALL="C"))
    log.write_text(result.stdout + result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--directory-account', action='store_true',
                       help='check mkdir/rmdir directory-count and unpublished-inode rollback')
    modes.add_argument('--mount-abort', action='store_true',
                       help='check callback/owner lifetime when journal mount fails')
    modes.add_argument('--directory-orphan', action='store_true',
                       help='check held-rmdir cleanup failure and fresh-process recovery')
    modes.add_argument('--unlink-rollback', action='store_true',
                       help='check failed dirent removal restores named final-link inodes')
    modes.add_argument('--hardlink-unlink', action='store_true',
                       help='check failed non-final unlink count restores both names')
    modes.add_argument('--hardlink-create', action='store_true',
                       help='check failed link count/publication and exhausted count rollback')
    modes.add_argument('--directory-grow', action='store_true',
                       help='check directory mapping and parent inode publication rollback')
    modes.add_argument('--create-rollback', action='store_true',
                       help='check file/symlink/mkdir initialization and publication rollback')
    parser.add_argument('--c-orphan', action='store_true',
                        help='use the C reference orphan controller')
    args = parser.parse_args()
    out = Path(os.environ['EVIDENCE_DIR']).resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'scope': 'shared Native backend with real file/block I/O; not Native ABI or guest crash certification'}
    try:
        modules = [('extent', 'kernel/fs/diskfs/ext4_extent_txn.rs'),
                   ('orphan', 'user/svc/ext4_orphan.rs'),
                   ('cache', 'user/svc/fscompat/bcache_user.rs')]
        wrapper = out / 'host.rs'
        wrapper.write_text('\n'.join(f'#[path="{ROOT/p}"] mod {n};' for n, p in modules) + '\n')
        library = out / 'libbackend.a'
        built = run(['rustc', '--edition=2021', '--crate-type=staticlib', '--cfg', 'host_std',
                     '-C', 'opt-level=1', wrapper, '-o', library], out / 'build-rust.log')
        if built.returncode:
            raise RuntimeError('Rust backend build failed')
        sources = ['kernel/fs/diskfs/' + n + '.c' for n in
                   ['ext4', 'ext4_namei', 'ext4_file', 'ext4_journal', 'ext4_csum', 'ext4_sync', 'ext4_xattr']]
        fixture = ('tools/tests/ext4_create_rollback/fixture.c' if args.create_rollback else
                   'tools/tests/ext4_directory_grow/fixture.c' if args.directory_grow else
                   'tools/tests/ext4_hardlink_create/fixture.c' if args.hardlink_create else
                   'tools/tests/ext4_hardlink_unlink/fixture.c' if args.hardlink_unlink else
                   'tools/tests/ext4_unlink_rollback/fixture.c' if args.unlink_rollback else
                   'tools/tests/ext4_directory_orphan/fixture.c' if args.directory_orphan else
                   'tools/tests/ext4_mount_abort/fixture.c' if args.mount_abort else
                   'tools/tests/ext4_directory_account/fixture.c' if args.directory_account
                   else 'tools/tests/ext4_orphan_compensation/fixture.c')
        sources += ['user/svc/fscompat/compat.c', 'user/svc/fscompat/bcache_user_rust_abi.c', fixture]
        binary = out / 'harness'
        orphan_config = [] if args.c_orphan else ['-DCONFIG_UFSD_RUST_EXT4_ORPHAN']
        built = run(['cc', '-std=gnu11', '-O1', '-DCONFIG_EXT4_RUST_EXTENT_TXN',
                     *orphan_config, '-ffunction-sections', '-fdata-sections',
                     '-Wno-builtin-declaration-mismatch', '-Iuser/liba20rt', '-Iuser/svc/fscompat',
                     '-Ikernel/include', '-Ikernel/arch/riscv64/include', *sources, library,
                     '-Wl,--gc-sections', '-lpthread', '-ldl', '-lm', '-o', binary], out / 'build-c.log')
        if built.returncode:
            raise RuntimeError('C backend build failed')
        inputs = sources + [p for _, p in modules] + [
            'kernel/fs/diskfs/tests/ext4_partial_trunc_host.c', 'kernel/include/fs/ext4.h']
        if args.hardlink_unlink:
            inputs.append('tools/tests/ext4_unlink_rollback/fixture.c')
        report['source_sha256'] = {p: sha(ROOT / p) for p in inputs}
        report['gate_sha256'] = sha(Path(__file__))
        report['harness_sha256'] = sha(binary)
        base, pattern = out / 'base.img', out / 'pattern'
        with base.open('wb') as stream:
            stream.truncate(32 * 1024 * 1024)
        features = ('has_journal,^metadata_csum,^64bit,^orphan_file' if args.mount_abort
                    else '^has_journal,^metadata_csum,^64bit,^orphan_file')
        made = run(['/usr/sbin/mkfs.ext4', '-q', '-F', '-b', '4096', '-O',
                    features, base], out / 'mkfs.log')
        if made.returncode:
            raise RuntimeError('mkfs failed')
        pattern.write_bytes(bytes((i * 37 + 19) % 251 for i in range(16384)))
        populated = run(['/usr/sbin/debugfs', '-w', '-R', f'write {pattern} single', base], out / 'populate.log')
        if populated.returncode or 'Allocated inode:' not in populated.stdout:
            raise RuntimeError('seed file was not created')
        report['base_sha256'] = sha(base)
        report['cases'] = []
        cases = ([('directory-orphan', [], 0)] if args.directory_orphan else
                 [('mount-abort', [], 0)] if args.mount_abort else
                 [('directories', [], 0)] if args.directory_account else
                 [('transient', [], 0), ('persistent', ['persistent'], 117)])
        for name, extra, expected in cases:
            image = out / (name + '.img')
            shutil.copyfile(base, image)
            result = run([binary, image, *extra], out / (name + '.log'))
            if result.returncode != expected:
                raise RuntimeError(f'{name}: unexpected backend exit {result.returncode}')
            recovery = None
            if args.directory_orphan:
                pending = re.search(r'DIR_ORPHAN_PENDING ino=(\d+) blocks=(\d+) inodes=(\d+)',
                                    result.stdout + result.stderr)
                if not pending:
                    raise RuntimeError('missing held directory orphan/checkpoint')
                raw = image.read_bytes()[1024:2048]
                ino = int(pending[1])
                if int.from_bytes(raw[0xe8:0xec], 'little') != ino or int.from_bytes(raw[0x3a:0x3c], 'little') & 3:
                    raise RuntimeError('held directory orphan/dirty state not persisted')
                recovery = run([binary, image, 'recover', pending[2], pending[3]],
                               out / 'directory-recovery.log')
                if recovery.returncode:
                    raise RuntimeError(f'directory recovery exited {recovery.returncode}')
                result = recovery
            marker = ('DIR_ORPHAN_RECOVERY_PASS' if args.directory_orphan else
                      'MOUNT_ABORT_PASS' if args.mount_abort else
                      'DIR_ACCOUNT_PASS' if args.directory_account else
                      'ORPHAN_RETRY_PASS' if not extra else 'ORPHAN_QUARANTINE_PASS')
            if marker not in (result.stdout + result.stderr).splitlines():
                raise RuntimeError(f'{name}: missing completed backend operation')
            checked = run(['/usr/sbin/e2fsck', '-fn', image], out / (name + '.fsck'))
            post = sha(image)
            if not extra and checked.returncode:
                raise RuntimeError('transient compensation left a corrupt postimage')
            if extra:
                # Inconsistent disk state is refused, not claimed to be fixed.
                state = int.from_bytes(image.read_bytes()[1024 + 0x3a:1024 + 0x3c], 'little')
                if not state & 2 or state & 1:
                    raise RuntimeError('fatal quarantine was not persisted in s_state')
                cold = run([binary, image, 'verify-quarantine'], out / 'cold-quarantine.log')
                if cold.returncode or 'ORPHAN_COLD_QUARANTINE_PASS' not in (cold.stdout + cold.stderr).splitlines():
                    raise RuntimeError('fresh backend admitted the quarantined image')
                if sha(image) != post:
                    raise RuntimeError('rejected mount changed the quarantined image')
            report['cases'].append({'name': name, 'backend_exit': result.returncode,
                                    'e2fsck_exit': checked.returncode, 'postimage_sha256': post})
        report['directory_orphan'] = args.directory_orphan
        report['orphan_controller'] = 'c' if args.c_orphan else 'rust'
        report['mount_abort'] = args.mount_abort
        report['directory_account'] = args.directory_account
        report['unlink_rollback'] = args.unlink_rollback
        report['hardlink_unlink'] = args.hardlink_unlink
        report['hardlink_create'] = args.hardlink_create
        report['directory_grow'] = args.directory_grow
        report['create_rollback'] = args.create_rollback
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('EXT4_ORPHAN_COMPENSATION: ' + ('PASS' if report['passed'] else report['error']))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
