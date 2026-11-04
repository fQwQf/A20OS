"""Platform-bus matching for static board devices.

The MCU board has no devicetree: devices carry a software_id and drivers carry
a firmware_device_id table.  This fixture pins the matching rule (including
driver_data propagation) and the IRQ resource validation used by probe().
"""
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

C_FIXTURE = r'''
#include <assert.h>
#include <string.h>
#include "core/types.h"
#include "core/errno.h"
#include "firmware/node.h"
#include "drivers/bus/platform_bus.h"

/* No firmware tree on this host: a static device is identified by
 * software_id. */
int fw_node_valid(const fw_node_t *n) { (void)n; return 0; }
const char *fw_node_source(const fw_node_t *n) { (void)n; return (void *)0; }
int fw_node_match_id(const fw_node_t *n, const char *id)
{
    (void)n; (void)id; return 0;
}

int bus_register(bus_type_t *b) { (void)b; return 0; }
int device_register(device_t *d) { (void)d; return 0; }
resource_t *device_get_resource(device_t *d, enum resource_type type, int index)
{
    for (int i = 0; i < d->res_count; i++) {
        if (d->res[i].type != type)
            continue;
        if (!index--)
            return &d->res[i];
    }
    return (void *)0;
}

#include "drivers/bus/platform_bus.c"

static const firmware_device_id_t ids[] = {
    {.id = "acme,gpio", .driver_data = 7},
    {.id = "acme,i2c0", .driver_data = 0},
    {.id = "acme,i2c1", .driver_data = 1},
    {.id = 0},
};

int main(void)
{
    driver_t drv = {.name = "test", .bus = &platform_bus,
                    .firmware_ids = ids};

    device_t gpio = {.name = "gpio", .software_id = "acme,gpio"};
    assert(platform_match(&gpio, &drv) == 1);
    assert(gpio.firmware_match == &ids[0]);
    assert(gpio.firmware_match->driver_data == 7);

    device_t i2c1 = {.name = "i2c1", .software_id = "acme,i2c1"};
    assert(platform_match(&i2c1, &drv) == 1);
    assert(i2c1.firmware_match->driver_data == 1);

    device_t other = {.name = "other", .software_id = "acme,spi0"};
    assert(platform_match(&other, &drv) == 0);
    assert(other.firmware_match == (void *)0);

    resource_t irq = {.type = RES_IRQ, .start = 45, .end = 45};
    device_t with_irq = {.name = "irq", .bus = &platform_bus,
                          .res = &irq, .res_count = 1};
    assert(platform_device_irq(&with_irq) == 45);
    irq.end = 46;
    assert(platform_device_irq(&with_irq) == -EINVAL);
    irq.end = 300;
    irq.start = 300;
    assert(platform_device_irq(&with_irq) == -ERANGE);
    device_t no_irq = {.name = "polled", .bus = &platform_bus};
    assert(platform_device_irq(&no_irq) == -ENODEV);

    extern int puts(const char *);
    puts("platform-bus: PASS");
    return 0;
}
'''


class PlatformBusMatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc"):
            raise unittest.SkipTest("host C compiler required")
        machine = platform.machine().lower()
        if machine in ("x86_64", "amd64"):
            arch, macro = "x86_64", "CONFIG_X86_64"
        elif machine in ("aarch64", "arm64"):
            arch, macro = "aarch64", "CONFIG_AARCH64"
        else:
            raise unittest.SkipTest(f"no host arch header for {machine}")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        source = work / "fixture.c"
        source.write_text(C_FIXTURE)
        cls.exe = work / "fixture"
        result = subprocess.run(
            ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
             "-fno-builtin", "-ffunction-sections", "-fdata-sections",
             "-fsanitize=undefined", "-fno-sanitize-recover=all",
             "-Wl,--gc-sections",
             f"-D{macro}", "-DCONFIG_NR_CPUS=1",
             f"-I{ROOT}/kernel/arch/{arch}/include",
             f"-I{ROOT}/kernel/include", f"-I{ROOT}/kernel",
             str(source), "-o", str(cls.exe)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def test_static_device_match_and_irq(self):
        result = subprocess.run([str(self.exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("platform-bus: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
