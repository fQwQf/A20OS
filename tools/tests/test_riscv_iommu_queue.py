"""Exercise production IOMMU CQ publication with delayed fake hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

if __package__:
    from .test_firmware_model import ROOT, STUBS
else:
    from test_firmware_model import ROOT, STUBS


class RiscvIommuQueue(unittest.TestCase):
    def test_timeout_late_completion_full_wrap_and_error(self):
        with tempfile.TemporaryDirectory(prefix="a20-iommu-queue-") as temp:
            work = Path(temp)
            stubs = {**STUBS,
                     "core/arch.h": "#define PAGE_OFFSET 0x100000000UL\n",
                     "core/defs.h": '''#pragma once
#include "core/types.h"
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
''',
                     "drivers/core/driver_hwapi.h": '''#pragma once
#include "core/types.h"
uint32_t host_read32(const volatile void *);
void host_write32(uint32_t, volatile void *);
struct device;
uint64_t dma_get_mask(const struct device *);
void dma_sync_for_device(void *, size_t);
void dma_sync_for_cpu(void *, size_t);
uint64_t clock_get_ticks(void);
uint64_t clock_ticks_per_sec(void);
#define readl host_read32
#define writel host_write32
'''}
            for name, content in stubs.items():
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = work / "test"
            subprocess.run([
                "cc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-DCONFIG_IOMMU_TRRESP_LEGACY_PPN",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-I" + str(work), "-I" + str(ROOT / "kernel/include"),
                "-I" + str(ROOT / "kernel"),
                str(ROOT / "tools/tests/riscv_iommu_queue_host.c"), "-o", str(binary),
                str(ROOT / "kernel/drivers/core/dma_mapping.c"),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
