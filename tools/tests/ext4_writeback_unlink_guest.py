"""Exercise fsync writeback against concurrent open-file unlink and chmod."""
import os
import subprocess
import tempfile
import traceback


def worker(directory):
    os.mkdir(directory)
    for index in range(12):
        name = directory + '/file'
        fd = os.open(name, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        try:
            # Force a stable inode with open-context metadata before unlink.
            assert os.write(fd, b'A' * 4096) == 4096
            os.fsync(fd)
            start_r, start_w = os.pipe()
            writer = os.fork()
            if writer == 0:
                os.close(start_w)
                try:
                    assert os.read(start_r, 1) == b'G'
                    for lap in range(12):
                        offset = (lap + 1) * 4096
                        assert os.pwrite(fd, bytes([lap + 1]) * 4096, offset) == 4096
                        os.fsync(fd)
                except BaseException:
                    traceback.print_exc()
                    os._exit(1)
                os._exit(0)
            os.close(start_r)
            os.write(start_w, b'G')
            os.close(start_w)
            os.unlink(name)
            assert os.fstat(fd).st_nlink == 0
            os.fchmod(fd, 0o640)
            _, status = os.waitpid(writer, 0)
            assert status == 0, status
            assert os.fstat(fd).st_nlink == 0, 'writeback resurrected an unlinked inode'
            assert os.pread(fd, 4096, 12 * 4096) == b'\x0c' * 4096
        finally:
            os.close(fd)
    os.rmdir(directory)


os.makedirs('/root/.cache', exist_ok=True)
with tempfile.TemporaryDirectory(prefix='/root/.cache/a20-writeback-') as base:
    children = []
    for number in range(4):
        pid = os.fork()
        if pid == 0:
            try:
                worker(base + '/' + str(number))
            except BaseException:
                traceback.print_exc()
                print('A20_TEST_FATAL: writeback/unlink worker failed', flush=True)
                os._exit(1)
            os._exit(0)
        children.append(pid)
    for pid in children:
        _, status = os.waitpid(pid, 0)
        assert status == 0, (pid, status)
ring = subprocess.check_output(['dmesg', '-s', '1048576'], text=True)
for marker in ('poison caller=', 'orphan reap preflight', 'deferred orphan cleanup failed'):
    assert marker not in ring, marker
print('WRITEBACK_UNLINK: PASS 48 concurrent unlink/576 fsync writes', flush=True)
