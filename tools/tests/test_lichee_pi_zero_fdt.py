"""Run the production V3s ARM32 firmware parser on host DTB fixtures.

Hardware operations are stubbed; these tests do not emulate or validate V3s.
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


def fixture(memory=0x4000000, compatible="licheepi,licheepi-zero",
            reservation=None, reserved_memory=None, malformed=False,
            dynamic_size=None, malformed_tail=False):
    t = Tree()
    t.begin("")
    t.text("compatible", compatible)
    t.cells("#address-cells", 1)
    t.cells("#size-cells", 1)
    # Deliberately put reservations before RAM: allocation must be independent
    # of firmware node order.
    if reserved_memory or dynamic_size:
        t.begin("reserved-memory")
        t.cells("#address-cells", 1)
        t.cells("#size-cells", 1)
        t.prop("ranges", b"")
        t.begin("firmware")
        if reserved_memory:
            t.cells("reg", *reserved_memory)
        if dynamic_size:
            t.cells("size", dynamic_size)
        t.end()
        t.end()
    t.begin("memory@40000000")
    t.text("device_type", "memory")
    t.cells("reg", 0x40000000, memory)
    t.end()
    t.begin("chosen")
    t.text("bootargs", "a20.hostname=lichee-pi-zero-test")
    t.end()
    t.end()
    data = t.finish([reservation] if reservation else [])
    if malformed:
        offset = struct.unpack_from(">I", data, 8)[0]
        return data[:offset] + b"\x00\x00\x00\xff" + data[offset + 4:]
    if malformed_tail:
        start, length = struct.unpack_from(">I", data, 8)[0], struct.unpack_from(">I", data, 36)[0]
        offset = start + length - 4
        return data[:offset] + b"\x00\x00\x00\xff" + data[offset + 4:]
    return data


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("cc"),
                     "host mmap harness requires Linux and cc")
class V3sFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="a20-lichee-pi-zero-")
        work = Path(cls.tmp.name)
        stubs = {
            # This suite executes early RAM/DT parsing only; storage electrical
            # setup has its own production-controller fixtures.
            "arch/common/sunxi_mmc0.h": "static inline void sunxi_board_prepare_mmc0(int a64) { (void)a64; }\n",
            # Audio MMIO/PLL/power runs in test_sunxi_audio, not a host RAM map.
            "arch/common/sunxi_audio.h": "#define SUNXI_AUDIO_A64 1\n#define SUNXI_AUDIO_V3S 2\nstatic inline int sunxi_board_prepare_audio(unsigned v, int c) { (void)v; (void)c; return 0; }\n",
            "core/arch.h": '''#pragma once
#include "core/types.h"
#define PAGE_OFFSET 0UL
#include "board_platform.h"
void arch_mb(void);
void arch_uart_flush(void);
void arch_halt(void);
''',
            "core/panic.h": '__attribute__((noreturn)) void panic(const char *, ...);\n',
            "core/smp.h": '#include "core/types.h"\nuint64_t arch_smp_boot_hw_id(void);\n',
            "core/stdio.h": '#include <stdio.h>\n',
            "core/timer.h": '#include "core/types.h"\nuint64_t timer_get_ticks(void);\n',
            "drivers/core/driver_core.h": '''#pragma once
#include "core/types.h"
typedef struct { void (*enable_irq)(uint32_t), (*disable_irq)(uint32_t); uint32_t (*ack)(void); void (*eoi)(uint32_t); } irqchip_ops_t;
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
    void (*configure_devices)(void);
    int defer_network_init, vdso_user_timer_unreliable;
} board_config_t;
extern const board_config_t *const current_board;
''',
        }
        stubs["mm/frame.h"] = "#pragma once\n#define PFA_MAX_RANGES 32\n"
        for name, content in stubs.items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        (work / "host.c").write_text('''#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include "boot/context.h"
#include "firmware/devicetree.h"
static boot_context_t test_context;
static char command_line[1024];
const boot_context_t *boot_context_get(void) { return &test_context; }
#include "drivers/core/driver_core.h"
uint32_t arm32_boot_dtb = 0x41800000;
uint64_t timer_get_ticks(void) { return 42; }
uint64_t arch_smp_boot_hw_id(void) { return 0; }
void arch_mb(void) {}
void arch_uart_flush(void) {}
void arch_halt(void) { exit(99); }
void firmware_shutdown(void) {}
void firmware_reboot(void) {}
void panic(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args);
    va_end(args); exit(42);
}
extern int v3s_ram_range(size_t, paddr_t *, paddr_t *);
extern const char *arch_bootargs_get(void);
int main(int argc, char **argv) {
    if (argc != 3) return 1;
    void *p = mmap((void *)0x41800000, 0x200000, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    size_t bytes = fread(p, 1, 0x200000, f); fclose(f);
    test_context.fdt = p; test_context.fdt_available = bytes;
    test_context.fdt_physical = 0x41800000;
    fw_dt_t tree;
    if (!fw_dt_init(&tree, p, bytes)) (void)fw_dt_bootargs(&tree, command_line, sizeof(command_line));
    test_context.command_line = command_line;
    current_board->early_init();
    paddr_t base = 0, end = 0;
    if (v3s_ram_range(0, &base, &end) || base != 0x40000000 ||
        end != strtoull(argv[2], NULL, 0) ||
        strcmp(arch_bootargs_get(), "a20.hostname=lichee-pi-zero-test") ||
        v3s_ram_range(32, &base, &end) >= 0) return 1;
    return 0;
}
''')
        cls.binary = work / "test"
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-no-pie",
            "-fno-builtin", "-D_start=v3s_test_start", "-D_end=v3s_test_end",
            "-Wl,--defsym,v3s_test_start=0x40080000",
            "-Wl,--defsym,v3s_test_end=0x40800000",
            "-I" + str(work), "-I" + str(ROOT / "kernel/include"),
            "-I" + str(ROOT / "kernel/arch/arm32/boards/lichee-pi-zero"),
            "-I" + str(ROOT / "kernel/arch/arm32/include"),
            str(work / "host.c"), str(ROOT / "kernel/core/fdt_view.c"),
            str(ROOT / "kernel/firmware/node.c"),
            str(ROOT / "kernel/firmware/devicetree/node.c"),
            str(ROOT / "kernel/firmware/devicetree/memory.c"), str(ROOT / "kernel/firmware/devicetree/framebuffer.c"), str(ROOT / "kernel/boot/framebuffer.c"),
            str(ROOT / "kernel/arch/arm32/boards/lichee-pi-zero/board.c"),
            "-o", str(cls.binary),
        ], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_fixture(self, data, expected_end=0x41800000, panic=False):
        path = Path(self.tmp.name) / "fixture.dtb"
        path.write_bytes(data)
        result = subprocess.run([str(self.binary), str(path), hex(expected_end)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 42 if panic else 0,
                         result.stdout + result.stderr)

    def test_64m_and_larger_firmware_ram_stay_bounded_by_dtb(self):
        self.run_fixture(fixture())
        self.run_fixture(fixture(memory=0x40000000))

    def test_reserved_memory_precedes_ram(self):
        self.run_fixture(fixture(reserved_memory=(0x41000000, 0x1000)),
                         expected_end=0x41000000)

    def test_fdt_memreserve(self):
        self.run_fixture(fixture(reservation=(0x41200000, 0x1000)),
                         expected_end=0x41200000)

    def test_reservation_over_kernel_is_rejected(self):
        self.run_fixture(fixture(reservation=(0x40080000, 0x1000)), panic=True)

    def test_wrong_board_is_rejected(self):
        self.run_fixture(fixture(compatible="allwinner,sun8i-v3s"), panic=True)

    def test_independently_mapped_dtb_may_be_outside_allocatable_ram(self):
        self.run_fixture(fixture(memory=0x1000000), expected_end=0x41000000)

    def test_malformed_tree_is_rejected(self):
        self.run_fixture(fixture(malformed=True), panic=True)

    def test_malformed_tail_after_valid_ram_is_rejected(self):
        self.run_fixture(fixture(malformed_tail=True), panic=True)

    def test_dynamic_firmware_reservation_is_rejected(self):
        self.run_fixture(fixture(dynamic_size=0x100000), panic=True)

    def test_static_reg_takes_precedence_over_optional_size(self):
        self.run_fixture(fixture(reserved_memory=(0x41000000, 0x1000), dynamic_size=0x100000),
                         expected_end=0x41000000)


if __name__ == "__main__":
    unittest.main()
