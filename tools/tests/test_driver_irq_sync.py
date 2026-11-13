"""Block production IRQ handlers/EOI while synchronizing and unregistering."""
from pathlib import Path
import subprocess
import tempfile
import unittest

if __package__:
    from .test_driver_workqueue import LOCK_STUB
else:
    from test_driver_workqueue import LOCK_STUB

ROOT = Path(__file__).resolve().parents[2]


class DriverIrqSync(unittest.TestCase):
    def test_complete_dispatch_and_shared_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="a20-irq-sync-") as temp:
            work = Path(temp)
            stubs = {
                "core/lock.h": LOCK_STUB,
                "core/defs.h": '''#pragma once
#include "core/types.h"
#define rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
''',
                "drivers/core/driver_core.h": '''#pragma once
#include "core/types.h"
struct device { uint64_t dma_mask; };
typedef struct { void (*enable_irq)(uint32_t); void (*disable_irq)(uint32_t);
 uint32_t (*ack)(void); void (*eoi)(uint32_t); } irqchip_ops_t;
typedef struct { uint64_t (*read_ticks)(void); uint64_t (*ticks_per_sec)(void); } timer_ops_t;
typedef struct { const irqchip_ops_t *irqchip; const timer_ops_t *timer; } board_config_t;
''',
                "mm/mm.h": '''#pragma once
#include "core/types.h"
static inline uint64_t va_to_pa(const void *p) { return (uintptr_t)p; }
void arch_dma_sync_for_device(void *, size_t);
void arch_dma_sync_for_cpu(void *, size_t);
''',
                "mm/frame.h": '''#pragma once
#include "core/types.h"
#define PAGE_SIZE 4096U
#define MAX_ORDER 11
#define PFN_NONE ((uint32_t)-1)
typedef uint32_t pfn_t;
pfn_t pfa_alloc(int); pfn_t pfa_alloc_max(int, uint64_t); void pfa_free(pfn_t, int);
void *pfn_to_virt(pfn_t); uint64_t pfn_to_phys(pfn_t); pfn_t virt_to_pfn(const void *);
''',
            }
            for name, content in stubs.items():
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = work / "test"
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-I" + str(work), "-I" + str(ROOT / "kernel/include"),
                "-I" + str(ROOT / "kernel"),
                str(ROOT / "tools/tests/driver_irq_sync_host.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
