"""Check the production read-only RTC probe's MMIO bounds and lifetime."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GoldfishRtcProbe(unittest.TestCase):
    def test_snapshot_window_and_mapping_release(self):
        production = re.sub(r'^#include[^\n]*\n', '',
                            (ROOT / 'kernel/drvmod/examples/goldfish_rtc.c').read_text(),
                            flags=re.M)
        prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#define A20_DRIVER_DESCRIPTOR(...)
#define RES_MMIO 1
#define DEV_CLASS_NONE 0
typedef struct { uint64_t start, end; } resource_t;
typedef struct { int present; resource_t resource; } device_t;
typedef struct { const char *id; } firmware_device_id_t;
typedef struct {
    const char *name; const firmware_device_id_t *firmware_ids; void *bus;
    int read_only_probe; int (*probe)(device_t *), (*remove)(device_t *);
    void (*shutdown)(device_t *); int class_type;
} driver_t;
static int platform_bus, maps, unmaps, reads, fail_map, active;
static unsigned long long logged;
static resource_t *device_get_resource(device_t *d, int type, int index) {
    assert(type == RES_MMIO && index == 0); return d->present ? &d->resource : NULL;
}
static uintptr_t drv_map_mmio(void *d, uintptr_t pa, size_t size) {
    assert(d && !(pa & 3) && size == 8 && !active); maps++;
    if (fail_map) return 0;
    active = 1; return 0x1000;
}
static void drv_unmap_mmio(void *d) { assert(d); if (active) unmaps++; active = 0; }
static uint32_t drv_read32(void *d, uintptr_t off) {
    assert(d && active && off == (reads % 2 ? 4u : 0u)); reads++;
    return off ? 0x12345678 : 0xabcdef01;
}
static void drv_log(const char *format, ...) {
    if (!strstr(format, "probe ok")) return;
    assert(!active && strstr(format, "epoch_ns=%llu"));
    va_list args; va_start(args, format); logged = va_arg(args, unsigned long long); va_end(args);
}
static int drv_driver_register(driver_t *driver) { assert(driver->read_only_probe); return 0; }
'''
        main = r'''
int main(void) {
    assert(!DriverEntry());
    device_t d = {0};
    assert(rtc_probe(NULL) == -ENODEV && rtc_probe(&d) == -ENODEV);
    d.present = 1;
    const resource_t bad[] = {{8, 7}, {0x101000, 0x101006}, {1, 8}};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        d.resource = bad[i]; assert(rtc_probe(&d) == -ENODEV);
    }
    assert(!maps && !reads);
    d.resource = (resource_t){0x101000, 0x101fff}; fail_map = 1;
    assert(rtc_probe(&d) == -EIO && !active && !reads && !unmaps);
    fail_map = 0;
    assert(!rtc_probe(&d) && !active && reads == 2 && unmaps == 1);
    assert(logged == 0x12345678abcdef01ULL);
    assert(!rtc_remove(&d) && unmaps == 1);
    d.resource.end = d.resource.start + 7;
    assert(!rtc_probe(&d) && !active && reads == 4 && unmaps == 2);
    /* A full-width declaration cannot wrap the mapped size to zero. */
    d.resource = (resource_t){0, UINTPTR_MAX};
    assert(!rtc_probe(&d) && !active && reads == 6 && unmaps == 3);
    assert(!rtc_remove(NULL));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='a20-goldfish-probe-') as tmp:
            source = Path(tmp) / 'probe.c'
            source.write_text(prefix + production + main)
            binary = source.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-fno-pie', '-no-pie', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)
