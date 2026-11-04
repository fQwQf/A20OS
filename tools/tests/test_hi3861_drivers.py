"""Host register-level tests for the ported Hi3861 drivers.

The drivers are compiled freestanding for rv32imc in the kernel, but their
register programming is architecture-independent.  This fixture includes the
real driver translation units and maps each platform resource to a plain host
buffer, so probe()/start()/read() can be checked without the physical board.
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Replace the kernel synchronization header with a single-threaded host
 * implementation, as the existing driver-core host test does. */
#define _CORE_SYNC_H
typedef struct { unsigned held; } mutex_t;
#define MUTEX_INIT {0}
#include "drivers/bus/platform_bus.h"

/* Host substitutes for the kernel services the drivers call. */
static void *host_alloc(size_t n, size_t s)
{
    return calloc(n, s);
}
void *kcalloc(size_t n, size_t s) { return host_alloc(n, s); }
void kfree(void *p) { free(p); }
void *device_map_mmio(device_t *dev, int index)
{
    return (void *)(uintptr_t)dev->res[index].start;
}
void udelay(unsigned us) { (void)us; }
void mdelay(unsigned ms) { (void)ms; }
bus_type_t platform_bus = {.name = "platform"};

void mutex_init(mutex_t *m) { m->held = 0; }
void mutex_lock(mutex_t *m) { assert(!m->held); m->held = 1; }
void mutex_unlock(mutex_t *m) { assert(m->held); m->held = 0; }

/* The driver core is not linked here; the driver_t objects only need the type
 * and the platform bus symbol. */
#include "drivers/gpio/hi3861_gpio.c"
#include "drivers/i2c/hi3861_i2c.c"
#include "drivers/spi/hi3861_spi.c"
#include "drivers/pwm/hi3861_pwm.c"
#include "drivers/adc/hi3861_adc.c"

static uint8_t gpio_regs[0x100] __attribute__((aligned(4)));
static uint8_t iocfg_regs[0x1000] __attribute__((aligned(4)));
static uint8_t i2c_regs[0x100] __attribute__((aligned(4)));
static uint8_t spi_regs[0x100] __attribute__((aligned(4)));
static uint8_t pwm_regs[0x600] __attribute__((aligned(4)));
static uint8_t adc_regs[0x100] __attribute__((aligned(4)));
static uint8_t crg_regs[0x100] __attribute__((aligned(4)));

static resource_t res[2];

static void set_res(device_t *dev, void *a, size_t alen, void *b, size_t blen)
{
    res[0] = (resource_t){.type = RES_MMIO, .start = (uintptr_t)a,
                          .end = (uintptr_t)a + alen - 1};
    res[1] = (resource_t){.type = RES_MMIO, .start = (uintptr_t)b,
                          .end = (uintptr_t)b + blen - 1};
    dev->res = res;
    dev->res_count = 2;
}

static uint16_t reg16(void *base, unsigned off)
{
    return *(volatile uint16_t *)((uint8_t *)base + off);
}
static uint32_t reg32(void *base, unsigned off)
{
    return *(volatile uint32_t *)((uint8_t *)base + off);
}

static void test_gpio(void)
{
    memset(gpio_regs, 0, sizeof(gpio_regs));
    memset(iocfg_regs, 0, sizeof(iocfg_regs));
    device_t dev = {.name = "hi3861-gpio"};
    set_res(&dev, gpio_regs, sizeof(gpio_regs), iocfg_regs, sizeof(iocfg_regs));
    assert(hi3861_gpio_driver.probe(&dev) == 0);

    assert(gpio_set_direction(3, GPIO_DIR_OUT) == 0);
    assert((reg16(gpio_regs, 0x04) & (1U << 3)) != 0);
    assert(gpio_set_value(3, 1) == 0);
    assert((reg16(gpio_regs, 0x00) & (1U << 3)) != 0);
    assert(gpio_get_direction(3) == GPIO_DIR_OUT);

    *(volatile uint16_t *)(gpio_regs + 0x50) = (uint16_t)(1U << 3);
    assert(gpio_get_value(3) == 1);

    assert(gpio_set_pull(3, GPIO_PULL_UP) == 0);
    assert(((reg32(iocfg_regs, 0x904 + 3 * 4) >> 8) & 3) == GPIO_PULL_UP);
    assert(gpio_set_function(3, 2) == 0);
    assert(reg32(iocfg_regs, 0x604 + 3 * 4) == 2);
    assert(gpio_set_input_enable(3, 1) == 0);
    assert((reg32(iocfg_regs, 0x904 + 3 * 4) & (1U << 10)) != 0);

    assert(gpio_set_value(15, 1) < 0);       /* out of range */
    assert(hi3861_gpio_driver.remove(&dev) == 0);
}

static void test_pwm(void)
{
    memset(pwm_regs, 0, sizeof(pwm_regs));
    memset(crg_regs, 0, sizeof(crg_regs));
    device_t dev = {.name = "hi3861-pwm"};
    set_res(&dev, pwm_regs, sizeof(pwm_regs), crg_regs, sizeof(crg_regs));
    assert(hi3861_pwm_driver.probe(&dev) == 0);

    assert(pwm_start(2, 250, 1000) == 0);
    void *ch = pwm_regs + 2 * 0x100;
    assert(reg16(ch, 0x00) == 1);
    assert(reg16(ch, 0x08) == 1000);
    assert(reg16(ch, 0x0C) == 250);
    assert(reg16(ch, 0x04) == 1);
    uint16_t gate = *(volatile uint16_t *)(crg_regs + 0x34);
    assert((gate & (1U << 2)) && (gate & (1U << 5)) && (gate & (1U << 6)));
    assert(pwm_start(2, 1001, 1000) < 0);
    assert(pwm_stop(2) == 0);
    assert(reg16(ch, 0x00) == 0);
    assert(hi3861_pwm_driver.remove(&dev) == 0);
}

static void test_adc(void)
{
    memset(adc_regs, 0, sizeof(adc_regs));
    memset(crg_regs, 0, sizeof(crg_regs));
    device_t dev = {.name = "hi3861-adc"};
    set_res(&dev, adc_regs, sizeof(adc_regs), crg_regs, sizeof(crg_regs));
    assert(hi3861_adc_driver.probe(&dev) == 0);

    *(volatile uint32_t *)(adc_regs + 0x10) = 1;                     /* RNE */
    *(volatile uint32_t *)(adc_regs + 0x24) = (3U << 12) | 0x456U;   /* ch3 */
    uint16_t raw = 0;
    assert(adc_read_raw(3, &raw) == 0);
    assert(raw == 0x456);
    assert(adc_raw_to_mv(0x456) == 1951);
    assert(adc_read_raw(7, &raw) < 0);
    assert(hi3861_adc_driver.remove(&dev) == 0);
}

static void test_i2c(void)
{
    memset(i2c_regs, 0, sizeof(i2c_regs));
    memset(crg_regs, 0, sizeof(crg_regs));
    device_t dev = {.name = "hi3861-i2c0"};
    set_res(&dev, i2c_regs, sizeof(i2c_regs), crg_regs, sizeof(crg_regs));
    dev.firmware_match = &hi3861_i2c_ids[0];
    assert(hi3861_i2c_driver.probe(&dev) == 0);
    assert(((*(volatile uint16_t *)(crg_regs + 0x30)) & (1U << 14)) != 0);
    assert(reg32(i2c_regs, 0x10) == 199);   /* 100 kHz from 80 MHz */
    assert(reg32(i2c_regs, 0x14) == 199);

    /* A DONE=1, ACK=0 status makes each polled phase complete. */
    *(volatile uint32_t *)(i2c_regs + 0x0C) = 1;
    uint8_t v = 0x5A;
    assert(i2c_write(0, 0x50, &v, 1) == 0);
    assert(reg32(i2c_regs, 0x18) == 0x5A);

    *(volatile uint32_t *)(i2c_regs + 0x0C) = 1;
    *(volatile uint32_t *)(i2c_regs + 0x1C) = 0x77;
    uint8_t r = 0;
    assert(i2c_read(0, 0x50, &r, 1) == 0);
    assert(r == 0x77);
    assert(i2c_write(0, 0xF050, &v, 1) == -EOPNOTSUPP);
    assert(hi3861_i2c_driver.remove(&dev) == 0);
}

static void test_spi(void)
{
    memset(spi_regs, 0, sizeof(spi_regs));
    memset(crg_regs, 0, sizeof(crg_regs));
    device_t dev = {.name = "hi3861-spi0"};
    set_res(&dev, spi_regs, sizeof(spi_regs), crg_regs, sizeof(crg_regs));
    dev.firmware_match = &hi3861_spi_ids[0];
    assert(hi3861_spi_driver.probe(&dev) == 0);
    assert(((*(volatile uint16_t *)(crg_regs + 0x30)) & (1U << 5)) != 0);
    assert((reg16(spi_regs, 0x00) & 0xF) == 7);  /* 8-bit frame */
    assert((reg16(spi_regs, 0x04) & (1U << 1)) != 0);
    assert((reg16(spi_regs, 0x10) & 1) == 0);

    *(volatile uint16_t *)(spi_regs + 0x0C) = 1;  /* TFE, not busy */
    uint8_t tx[4] = {1, 2, 3, 0xA5};
    uint8_t rx[4] = {0};
    assert(spi_transfer(0, tx, rx, sizeof(tx), 1000000, 0) == 0);
    assert(rx[3] == 0xA5);   /* model DR: reads return the last written byte */
    assert(hi3861_spi_driver.remove(&dev) == 0);
}

int main(void)
{
    test_gpio();
    test_pwm();
    test_adc();
    test_i2c();
    test_spi();
    extern int puts(const char *);
    puts("hi3861-drivers: PASS");
    return 0;
}
'''


class Hi3861DriverTests(unittest.TestCase):
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
             "-fno-builtin", "-fsanitize=undefined", "-fno-sanitize-recover=all",
             f"-D{macro}", "-DCONFIG_NR_CPUS=1",
             f"-I{ROOT}/kernel/arch/{arch}/include",
             f"-I{ROOT}/kernel/include", f"-I{ROOT}/kernel",
             str(source), "-o", str(cls.exe)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def test_register_programming(self):
        result = subprocess.run([str(self.exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hi3861-drivers: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
