"""Production diagnostic input probe and shared register protocol."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VinputProbeWindow(unittest.TestCase):
    def test_resource_window_and_stateless_retirement(self):
        prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define A20_DRIVER_DESCRIPTOR(...)
#define RES_MMIO 1
#define DEV_CLASS_NONE 0
typedef struct { uint64_t start, end; } resource_t;
typedef struct { int present; resource_t resource; } device_t;
typedef struct { const char *id; } firmware_device_id_t;
typedef struct {
    const char *name; const firmware_device_id_t *firmware_ids; void *bus;
    int read_only_probe; int (*probe)(device_t *), (*remove)(device_t *); int class_type;
} driver_t;
static int platform_bus, maps, unmaps, fail_map, active, writes;
static uint32_t registers[0x188 / 4];
static resource_t *device_get_resource(device_t *d, int type, int index) {
    assert(type == RES_MMIO && index == 0); return d->present ? &d->resource : NULL;
}
static uint64_t drv_mmio_map(uint64_t pa, uint64_t size, unsigned prot) {
    assert(!(pa & 3) && size == sizeof(registers) && prot == 3 && !active); maps++;
    if (fail_map) return 0;
    active = 1; return (uintptr_t)registers;
}
static void drv_mmio_unmap(uint64_t base, uint64_t size) {
    assert(active && base == (uintptr_t)registers && size == sizeof(registers)); active = 0; unmaps++;
}
static uint32_t drv_mmio_read32(uint64_t base, uint32_t off) {
    assert(active && base == (uintptr_t)registers && !(off & 3) && off < sizeof(registers));
    return registers[off / 4];
}
static uint8_t drv_mmio_read8(uint64_t base, uint32_t off) {
    assert(active && base == (uintptr_t)registers && off < sizeof(registers));
    return ((uint8_t *)registers)[off];
}
static void drv_mmio_write8(uint64_t base, uint32_t off, uint8_t value) {
    assert(active && base == (uintptr_t)registers && (off == 0x100 || off == 0x101));
    ((uint8_t *)registers)[off] = value; writes++;
}
static void drv_mmio_write32(uint64_t base, uint32_t off, uint32_t value) {
    (void)base; (void)off; (void)value; assert(0); /* no status/queue writes */
}
static void drv_log(const char *format, ...) { (void)format; }
static int drv_driver_register(driver_t *driver) {
    assert(driver->read_only_probe && driver->remove); return 0;
}
'''
        sources = []
        for name in ['kernel/include/drivers/dual/virtio_mmio.h',
                     'kernel/include/drivers/dual/virtio_input.h',
                     'kernel/drvmod/examples/vinput_probe.c']:
            sources.append(re.sub(r'^#include[^\n]*\n', '', (ROOT / name).read_text(), flags=re.M))
        main = r'''
int main(void) {
    assert(!DriverEntry());
    device_t d = {0};
    assert(vinput_probe(NULL) == -ENODEV && vinput_probe(&d) == -ENODEV);
    d.present = 1;
    const resource_t bad[] = {{8, 7}, {0xa000a00, 0xa000b86}, {1, 4096}};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        d.resource = bad[i]; assert(vinput_probe(&d) == -ENODEV);
    }
    assert(!maps && !unmaps);
    d.resource = (resource_t){0xa000a00, 0xa000bff}; fail_map = 1;
    assert(vinput_probe(&d) == -EIO && !active && !unmaps); fail_map = 0;
    assert(vinput_probe(&d) == -ENODEV && !active && unmaps == 1 && !writes);
    registers[VMMIO_MAGIC / 4] = VMMIO_MAGIC_VALUE;
    registers[VMMIO_VERSION / 4] = 2;
    registers[VMMIO_DEVICE_ID / 4] = VIRTIO_INPUT_DEVICE_ID;
    ((uint8_t *)registers)[VMMIO_CONFIG + VINPUT_CFG_SIZE] = 3;
    memcpy((uint8_t *)registers + VMMIO_CONFIG + VINPUT_CFG_DATA, "kbd", 3);
    assert(!vinput_probe(&d) && !active && unmaps == 2 && writes == 3);
    assert(!((uint8_t *)registers)[VMMIO_CONFIG + VINPUT_CFG_SELECT]);
    assert(!vinput_probe_driver.remove(&d) && !active && unmaps == 2);
    d.resource = (resource_t){0x10006000, 0x10006fff};
    assert(!vinput_probe(&d) && !active && unmaps == 3);
    d.resource.end = d.resource.start + VINPUT_PROBE_SIZE - 1;
    assert(!vinput_probe(&d) && !active && unmaps == 4);
    assert(!vinput_probe_driver.remove(NULL) && !active && unmaps == 4);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='a20-vinput-probe-') as tmp:
            source = Path(tmp) / 'probe.c'
            source.write_text(prefix + '\n'.join(sources) + main)
            binary = source.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-fno-pie', '-no-pie', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)
