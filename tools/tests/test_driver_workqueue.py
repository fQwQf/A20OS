"""Production queues with real host threads and deadline-backed Park/Wake."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
LOCK_STUB = '''#pragma once
#include <assert.h>
#include <pthread.h>
#include "core/types.h"
extern _Thread_local unsigned host_lock_depth;
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void spin_init(spinlock_t *l) { assert(!pthread_mutex_init(l, NULL)); }
static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
 assert(!pthread_mutex_lock(l)); host_lock_depth++; return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
 (void)flags; assert(host_lock_depth); host_lock_depth--; assert(!pthread_mutex_unlock(l));
}
'''


class DriverWorkqueue(unittest.TestCase):
    def test_delays_cancel_teardown_and_concurrent_start(self):
        with tempfile.TemporaryDirectory(prefix="a20-workqueue-") as temp:
            work = Path(temp)
            stubs = {
                "core/lock.h": LOCK_STUB,
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
                str(ROOT / "tools/tests/driver_workqueue_host.c"),
                str(ROOT / "kernel/drivers/core/workqueue.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
