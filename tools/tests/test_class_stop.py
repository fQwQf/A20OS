"""Native stop receipts are distinct from class offline and failed remove."""
from pathlib import Path
import tempfile
import subprocess
import unittest
from .test_firmware_model import ROOT, STUBS

class ClassStop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-class-stop-');cls.addClassCleanup(cls.temp.cleanup)
        work=Path(cls.temp.name);stubs=dict(STUBS)
        stubs['net/socket/socket_internal.h']='#include "core/types.h"\nvoid netlink_uevent_emit(const char *,const char *,const char *,uint64_t);\n'
        for name,body in stubs.items():
            p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
        cls.binary=work/'test'
        subprocess.run(['cc','-std=gnu11','-pthread','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(work),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),str(ROOT/'tools/tests/class_stop_host.c'),str(ROOT/'kernel/drivers/core/driver_core.c'),str(ROOT/'kernel/drivers/core/driver_class.c'),str(ROOT/'kernel/firmware/node.c'),'-o',str(cls.binary)],check=True)
    def run_case(self,name):subprocess.run([str(self.binary),name],check=True,timeout=15)
    def test_exact_action_release_detaches_once_and_preserves_order(self):self.run_case('actions')
    def test_stop_receipt_follows_successful_remove(self):self.run_case('success')
    def test_failed_remove_preserves_unstopped_binding_and_retry(self):self.run_case('failed-remove')
    def test_offline_and_call_drain_are_not_stop_receipts(self):self.run_case('drain')
    def test_receipt_does_not_follow_rebinding(self):self.run_case('rebind')
    def test_publication_and_action_oom_leave_no_owner(self):
        for at in range(2):
            with self.subTest(allocation=at):self.run_case('publish-oom-'+str(at))
    def test_missing_remove_is_not_a_stop_contract(self):self.run_case('missing-remove')
