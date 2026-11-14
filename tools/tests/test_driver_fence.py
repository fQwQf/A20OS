"""Real fence code with a pthread park backend and injected wake boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DriverFence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="a20-fence-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        stubs = {
            "core/lock.h": '''#pragma once
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
''',
            "drivers/core/driver_hwapi.h":
                '#include "core/types.h"\nuint64_t clock_get_ticks(void);\n',
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
            str(ROOT / "tools/tests/driver_fence_host.c"),
            str(ROOT / "kernel/drivers/core/fence.c"), "-o", str(binary),
        ], check=True)
        cls.binary = binary

    def run_case(self, *args):
        subprocess.run([str(self.binary), *args], check=True, timeout=20)

    def test_completion_timeout_and_concurrent_producers(self):
        self.run_case()

    def test_invalid_arguments_clear_output(self):
        self.run_case("arguments")

    def test_allocation_failure_keeps_fence_pending(self):
        self.run_case("allocation")

    def test_terminal_fence_rejects_late_registration_without_replay(self):
        self.run_case("terminal")

    def test_completion_during_watch_allocation_is_rechecked_before_publication(self):
        self.run_case("allocation-signal")

    def test_pending_registration_retains_fence_until_removed(self):
        self.run_case("pending-owner")

    def test_completed_registration_retains_fence_until_removed(self):
        self.run_case("complete-owner")

    def test_success_notifies_once_outside_lock_without_irq_allocation(self):
        self.run_case("notify")

    def test_error_status_reaches_notification_unchanged(self):
        self.run_case("error")

    def test_cancellation_prevents_only_this_notification(self):
        self.run_case("cancel")

    def test_many_watches_allow_head_tail_and_middle_cancellation(self):
        self.run_case("many")

    def test_running_callback_preserves_owner_until_retry(self):
        self.run_case("running")

    def test_terminal_result_does_not_prevent_cancelling_unselected_watch(self):
        self.run_case("cancel-terminal-pending")

    def test_waiters_wake_before_a_paused_notification_retires(self):
        self.run_case("waiter-progress")

    def test_registration_racing_completion_has_no_lost_notification(self):
        self.run_case("register-race")

    def test_removal_racing_signal_is_safe_and_at_most_once(self):
        self.run_case("remove-race")

    def test_competing_producers_deliver_one_consistent_result(self):
        self.run_case("producers")


if __name__ == "__main__":
    unittest.main()
