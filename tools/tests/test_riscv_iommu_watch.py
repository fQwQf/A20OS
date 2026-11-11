"""Production fault watcher and workqueue with real host worker threads."""
from pathlib import Path
import subprocess
import tempfile
import unittest

if __package__:
    from .test_driver_workqueue import ROOT, LOCK_STUB
else:
    from test_driver_workqueue import ROOT, LOCK_STUB


class RiscvIommuWatch(unittest.TestCase):
    def test_generation_notifications_and_stop_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="a20-iommu-watch-") as temp:
            work = Path(temp)
            stubs = {
                "core/lock.h": LOCK_STUB,
                "drivers/core/driver_core.h": '''#pragma once
#include "core/types.h"
typedef struct device { unsigned references; } device_t;
device_t *device_get(device_t *); void device_put(device_t *);
''',
                "drivers/core/driver_hwapi.h":
                    '#include "core/types.h"\nuint64_t clock_get_ticks(void);\n',
                "proc/proc.h": '''#pragma once
typedef struct task_t task_t;
task_t *proc_current(void);
int proc_alloc_detached(void (*)(void));
void proc_yield(void);
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
                "-I" + str(work), "-I" + str(ROOT / "kernel/include"),
                "-I" + str(ROOT / "kernel"),
                str(ROOT / "tools/tests/riscv_iommu_watch_host.c"),
                str(ROOT / "kernel/drivers/core/riscv_iommu_watch.c"),
                str(ROOT / "kernel/drivers/core/workqueue.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
