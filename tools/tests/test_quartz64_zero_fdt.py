"""Run the production Quartz64-Zero firmware parser on host DTB fixtures.

Hardware operations are stubbed; these tests do not emulate or validate A64.
"""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from raspberrypi4_fixture import Tree


def fixture(memory=0x40000000, compatible="pine64,quartz64-zero",
            reservation=None, reserved_memory=None, malformed=False,
            dynamic_size=None):
    t = Tree()
    t.begin("")
    t.text("compatible", compatible)
    t.cells("#address-cells", 1)
    t.cells("#size-cells", 1)
    # Deliberately put reservations before RAM: allocation must be independent
    # of firmware node order.
    if reserved_memory or dynamic_size is not None:
        t.begin("reserved-memory")
        t.cells("#address-cells", 1)
        t.cells("#size-cells", 1)
        t.prop("ranges", b"")
        t.begin("firmware@%x" % reserved_memory[0] if reserved_memory else "dynamic")
        if reserved_memory:
            t.cells("reg", *reserved_memory)
        if dynamic_size is not None:
            t.cells("size", dynamic_size)
        t.end()
        t.end()
    t.begin("memory@0")
    t.text("device_type", "memory")
    t.cells("reg", 0, memory)
    t.end()
    t.begin("chosen")
    t.text("bootargs", "a20.hostname=quartz64-zero-test")
    t.end()
    t.end()
    data = t.finish([reservation] if reservation else [])
    if malformed:
        offset = struct.unpack_from(">I", data, 8)[0]
        return data[:offset] + b"\x00\x00\x00\xff" + data[offset + 4:]
    return data


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("cc"),
                     "host mmap harness requires Linux and cc")
class Quartz64ZeroFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="a20-quartz64-zero-")
        work = Path(cls.tmp.name)
        stubs = {
            "core/arch.h": '''#pragma once
#include "core/types.h"
#define PAGE_OFFSET 0UL
#include "board_platform.h"
''',
            "core/panic.h": '__attribute__((noreturn)) void panic(const char *, ...);\n',
            "core/smp.h": '#include "core/types.h"\nuint64_t arch_smp_boot_hw_id(void);\n',
            "core/stdio.h": '#include <stdio.h>\n',
            "core/timer.h": '#include "core/types.h"\nuint64_t timer_get_ticks(void);\n',
            "drivers/core/driver_core.h": '''#pragma once
#include "core/types.h"
typedef struct { int unused; } irqchip_ops_t;
typedef struct {
    uint64_t (*read_ticks)(void), (*ticks_per_sec)(void);
} timer_ops_t;
typedef struct {
    const char *name;
    paddr_t ram_base, ram_end;
    const irqchip_ops_t *irqchip;
    const timer_ops_t *timer;
    void *smp;
    void (*early_init)(void), (*poweroff)(void), (*reboot)(void);
    int defer_network_init, vdso_user_timer_unreliable;
} board_config_t;
extern const irqchip_ops_t quartz64_zero_gic_ops;
extern const board_config_t *const current_board;
''',
        }
        for name, content in stubs.items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        (work / "host.c").write_text('''#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include "drivers/core/driver_core.h"
uint64_t aarch64_boot_dtb = 0x0a000000;
const irqchip_ops_t quartz64_zero_gic_ops = {0};
uint64_t aarch64_timer_freq(void) { return 24000000; }
uint64_t timer_get_ticks(void) { return 42; }
uint64_t arch_smp_boot_hw_id(void) { return 0; }
void firmware_shutdown(void) {}
void firmware_reboot(void) {}
void panic(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args);
    va_end(args); exit(42);
}
extern int quartz64_zero_ram_range(size_t, paddr_t *, paddr_t *);
extern const char *arch_bootargs_get(void);
int main(int argc, char **argv) {
    if (argc != 3) return 1;
    void *p = mmap((void *)0x0a000000, 0x200000, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fread(p, 1, 0x200000, f); fclose(f);
    current_board->early_init();
    paddr_t base = 0, end = 0;
    if (quartz64_zero_ram_range(0, &base, &end) || base != 0x00200000 ||
        end != strtoull(argv[2], NULL, 0) ||
        strcmp(arch_bootargs_get(), "a20.hostname=quartz64-zero-test") ||
        quartz64_zero_ram_range(1, &base, &end) != -1) return 1;
    return 0;
}
''')
        cls.binary = work / "test"
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-no-pie",
            "-D_start=quartz_test_start", "-D_end=quartz_test_end",
            "-Wl,--defsym,quartz_test_start=0x02080000",
            "-Wl,--defsym,quartz_test_end=0x02800000",
            "-I" + str(work), "-I" + str(ROOT / "kernel/include"),
            "-I" + str(ROOT / "kernel/platform/quartz64-zero"),
            "-I" + str(ROOT / "kernel/arch/aarch64/include"),
            str(work / "host.c"), str(ROOT / "kernel/core/fdt_view.c"),
            str(ROOT / "kernel/platform/quartz64-zero/board.c"),
            "-o", str(cls.binary),
        ], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_fixture(self, data, expected_end=0x0a000000, panic=False):
        path = Path(self.tmp.name) / "fixture.dtb"
        path.write_bytes(data)
        result = subprocess.run([str(self.binary), str(path), hex(expected_end)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 42 if panic else 0,
                         result.stdout + result.stderr)

    def test_1g_and_larger_firmware_ram_stay_bounded_by_dtb(self):
        self.run_fixture(fixture())
        self.run_fixture(fixture(memory=0x80000000))

    def test_reserved_memory_precedes_ram(self):
        self.run_fixture(fixture(reserved_memory=(0x08000000, 0x1000)),
                         expected_end=0x08000000)

    def test_fdt_memreserve(self):
        self.run_fixture(fixture(reservation=(0x07000000, 0x1000)),
                         expected_end=0x07000000)

    def test_reservation_over_kernel_is_rejected(self):
        self.run_fixture(fixture(reservation=(0x02080000, 0x1000)), panic=True)

    def test_wrong_board_is_rejected(self):
        self.run_fixture(fixture(compatible="pine64,quartz64-zero-plus"), panic=True)

    def test_dtb_beyond_reported_ram_is_rejected(self):
        self.run_fixture(fixture(memory=0x8000000), panic=True)

    def test_dynamic_reserved_memory_is_rejected(self):
        self.run_fixture(fixture(dynamic_size=0x100000), panic=True)

    def test_dynamic_size_with_reg_is_rejected_conservatively(self):
        self.run_fixture(fixture(reserved_memory=(0x08000000, 0x1000),
                                 dynamic_size=0x1000), panic=True)

    def test_missing_final_end_is_rejected_after_valid_memory(self):
        data = bytearray(fixture())
        size = struct.unpack_from(">I", data, 36)[0]
        struct.pack_into(">I", data, 36, size - 4)
        self.run_fixture(data, panic=True)

    def test_bad_final_token_is_rejected_after_valid_memory(self):
        data = bytearray(fixture())
        start = struct.unpack_from(">I", data, 8)[0]
        size = struct.unpack_from(">I", data, 36)[0]
        struct.pack_into(">I", data, start + size - 4, 0xff)
        self.run_fixture(data, panic=True)

    def test_truncated_late_property_is_rejected(self):
        data = bytearray(fixture())
        value = data.index(b"a20.hostname=quartz64-zero-test\0")
        struct.pack_into(">I", data, value - 8, 0xfffffff0)
        self.run_fixture(data, panic=True)

    def test_malformed_tree_is_rejected(self):
        self.run_fixture(fixture(malformed=True), panic=True)


if __name__ == "__main__":
    unittest.main()
