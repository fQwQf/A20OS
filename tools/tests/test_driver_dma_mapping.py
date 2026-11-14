"""Real native mappings: nonidentity addresses, page pins and failure rollback."""
from pathlib import Path
import subprocess
import tempfile
import unittest

if __package__:
    from .test_firmware_model import ROOT, STUBS
else:
    from test_firmware_model import ROOT, STUBS


class DriverDmaMapping(unittest.TestCase):
    def test_mapping_ownership_and_errors(self):
        self.run_harness("driver_dma_mapping_host.c")

    def test_managed_map_shutdown_serialization(self):
        self.run_harness("driver_dma_managed_host.c", threaded=True)

    def test_immutable_policy_and_managed_enforcement(self):
        self.run_harness("driver_dma_policy_host.c")

    def run_harness(self, harness, threaded=False):
        with tempfile.TemporaryDirectory(prefix="a20-dma-mapping-") as temp:
            work = Path(temp)
            stubs = {**STUBS,
                     "core/arch.h": "#define PAGE_OFFSET 0x100000000UL\n",
                     "drivers/core/driver_hwapi.h": '''#pragma once
#include "core/types.h"
struct device;
uint64_t dma_get_mask(const struct device *);
void dma_sync_for_device(void *, size_t);
void dma_sync_for_cpu(void *, size_t);
'''}
            if threaded:
                stubs["core/sync.h"] = '''#pragma once
#include <assert.h>
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
extern _Thread_local int host_closer;
extern int host_closing_wait;
static inline void mutex_init(mutex_t *m) { assert(!pthread_mutex_init(m, NULL)); }
static inline void mutex_lock(mutex_t *m) {
 if (host_closer) __atomic_store_n(&host_closing_wait, 1, __ATOMIC_RELEASE);
 assert(!pthread_mutex_lock(m));
}
static inline void mutex_unlock(mutex_t *m) { assert(!pthread_mutex_unlock(m)); }
'''
            for name, content in stubs.items():
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = work / "test"
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(work), "-I" + str(ROOT / "kernel/include"),
                "-I" + str(ROOT / "kernel"),
                str(ROOT / "tools/tests" / harness),
                str(ROOT / "tools/tests/dma_buffer_unused_host.c"),
                str(ROOT / "kernel/drivers/core/dma_mapping.c"), "-o", str(binary),
                str(ROOT / "kernel/drivers/core/dma_policy.c"),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
