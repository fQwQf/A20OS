"""Native request ownership, IRQ completion, task retirement and managed stop."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_driver_fence import build_fence_fixture, ROOT
from .test_firmware_model import STUBS

class DriverQueue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-queue-');cls.addClassCleanup(cls.temp.cleanup)
        source=(ROOT/'tools/tests/driver_fence_host.c').read_text().split('int main(int argc, char **argv)')[0]
        source+=(ROOT/'tools/tests/driver_queue_host.c').read_text()
        stubs={p:STUBS[p] for p in ['core/defs.h','core/cpu.h']}
        stubs['core/arch.h']='#define PAGE_OFFSET 0UL\n'
        cls.binary=build_fence_fixture(Path(cls.temp.name),source,
            [ROOT/'kernel/drivers/core/queue.c'],stubs)
    def workflow(self,name):subprocess.run([str(self.binary),name],check=True,timeout=20)
    def test_parallel_issue_out_of_order_irq_and_backpressure(self):self.workflow('parallel')
    def test_fifo_dependency_and_exact_queued_cancellation(self):self.workflow('cancel')
    def test_cleanup_failure_retains_capacity_and_stop_retries(self):self.workflow('retire')
    def test_shared_worker_backpressure_partial_prepare_and_inline_completion(self):self.workflow('worker')
    def test_owned_memory_and_fence_outlive_caller_and_failed_stop(self):self.workflow('memory')
    def test_close_racing_dispatch_returns_before_active_call_drain(self):self.workflow('race')

    def test_dependency_completion_wakes_worker_without_tick_polling(self):self.workflow('dependency-wake')
