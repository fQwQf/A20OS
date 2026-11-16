"""Compare getrandom boundary returns with Linux, then execute the real adapter."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.tests.test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static unsigned generated, copies, fail_copy;
static void random_fill(void *dst, size_t size) {
    generated++; memset(dst, 0x5a, size);
}
static long copy_to_user(void *dst, const void *src, size_t size) {
    if (++copies == fail_copy) return -EFAULT;
    memcpy(dst, src, size); return 0;
}
'''
HARNESS = r'''
int main(void) {
    /* Actual Linux reference: zero-sized NULL, invalid flags, and a buffer
     * with a writable 128-byte prefix followed by a protected page. */
    assert(syscall(SYS_getrandom, NULL, 0, 0) == 0);
    errno = 0;
    assert(syscall(SYS_getrandom, NULL, 0, 6) == -1 && errno == EINVAL);
    long page = sysconf(_SC_PAGESIZE);
    char *mapping = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    assert(mprotect(mapping + page, page, PROT_NONE) == 0);
    assert(syscall(SYS_getrandom, mapping + page - 128, 256, 0) == 128);
    assert(munmap(mapping, 2 * page) == 0);

    assert(sys_getrandom(NULL, 0, 0) == 0);
    assert(sys_getrandom(NULL, 0, LINUX_GRND_INSECURE) == 0);
    assert(sys_getrandom(NULL, 0, 8) == -EINVAL);
    assert(sys_getrandom(NULL, 0, LINUX_GRND_RANDOM | LINUX_GRND_INSECURE) == -EINVAL);
    assert(sys_getrandom(NULL, 1, 0) == -EFAULT);
    assert(!generated && !copies);
    unsigned char buffer[300];
    memset(buffer, 0xdd, sizeof(buffer));
    fail_copy = 1;
    assert(sys_getrandom(buffer, 256, 0) == -EFAULT);
    assert(buffer[0] == 0xdd);
    generated = copies = 0; fail_copy = 2;
    assert(sys_getrandom(buffer, 256, 0) == 128);
    assert(generated == 2 && copies == 2);
    for (unsigned i = 0; i < 128; i++) assert(buffer[i] == 0x5a);
    for (unsigned i = 128; i < sizeof(buffer); i++) assert(buffer[i] == 0xdd);
    generated = copies = fail_copy = 0;
    assert(sys_getrandom(buffer, 257, 0) == 257);
    assert(generated == 3 && copies == 3 && buffer[256] == 0x5a && buffer[257] == 0xdd);
    for (unsigned flags = 1; flags <= 4; flags <<= 1)
        assert(sys_getrandom(buffer, 1, flags) == 1);
    return 0;
}
'''


class GetrandomABI(unittest.TestCase):
    def test_zero_length_and_later_copy_fault(self):
        source = (ROOT / "kernel/abi/linux/sys_misc.c").read_text()
        constants = source[source.index("#define LINUX_GRND_NONBLOCK"):
                           source.index("int64_t sys_uname")]
        function = extract_function(source, "int64_t sys_getrandom(")
        with tempfile.TemporaryDirectory(prefix="a20-getrandom-") as directory:
            test = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            test.write_text(PRELUDE + constants + function + HARNESS)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
