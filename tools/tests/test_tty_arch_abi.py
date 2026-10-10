"""Exercise the production PowerPC termios codec against its Linux wire ABI."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TtyArchitectureAbiTest(unittest.TestCase):
    def test_powerpc_wire_offsets_and_usercopy_failure(self):
        harness = r'''
#include "core/types.h"
#include "core/string.h"
#include "core/ioctl.h"
#include "fs/tty.h"
extern void abort(void);
#define CHECK(x) do { if (!(x)) abort(); } while (0)
static int fail_copy;
long copy_to_user(void *dst, const void *src, size_t n) {
    if (fail_copy) return -14;
    memcpy(dst, src, n); return 0;
}
long copy_from_user(void *dst, const void *src, size_t n) {
    return copy_to_user(dst, src, n);
}
int main(void) {
    ktty_termios_t input = { .c_iflag=1, .c_oflag=2, .c_cflag=3,
                            .c_lflag=4, .c_line=7 }, output;
    for (unsigned i=0; i<KTTY_NCCS; i++) input.c_cc[i]=30+i;
    uint8_t wire[48];
    memset(wire, 0xa5, sizeof(wire));
    CHECK(arch_tty_termios_to_user(wire, &input) == 0);
    /* PowerPC has c_cc before c_line and two 32-bit speed fields. */
    CHECK(memcmp(wire+16, input.c_cc, KTTY_NCCS) == 0);
    CHECK(wire[35] == 7);
    for (unsigned i=36; i<44; i++) CHECK(wire[i] == 0);
    for (unsigned i=44; i<48; i++) CHECK(wire[i] == 0xa5);
    CHECK(arch_tty_termios_from_user(&output, wire) == 0);
    CHECK(memcmp(&input, &output, sizeof(input)) == 0);
    CHECK(arch_linux_ioctl_request(0x402c7413UL) == TCGETS);
    CHECK(arch_linux_ioctl_request(0x80047476UL) == TIOCSPGRP);
    CHECK(arch_linux_ioctl_request(0x40087468UL) == TIOCGWINSZ);
    CHECK(arch_linux_ioctl_request(0x123456UL) == 0x123456UL);
    fail_copy=1;
    CHECK(arch_tty_termios_to_user(wire, &input) == -14);
    memset(&output, 0xa5, sizeof(output));
    CHECK(arch_tty_termios_from_user(&output, wire) == -14);
    CHECK(output.c_iflag == 0xa5a5a5a5U);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / "test.c"
            exe = Path(temp) / "test"
            src.write_text(harness)
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "kernel/include"), str(src),
                            str(ROOT / "kernel/arch/ppc64le/abi/tty_abi.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
