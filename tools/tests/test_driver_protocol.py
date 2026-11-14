"""Native multi-protocol services, legacy adapters and MCU share one binding core."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from .test_class_binding import HARNESS
from .test_firmware_model import ROOT, STUBS


class DriverProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-protocol-');cls.addClassCleanup(cls.temp.cleanup)
        work=Path(cls.temp.name);stubs=dict(STUBS)
        stubs['net/socket/socket_internal.h']='#include "core/types.h"\nvoid netlink_uevent_emit(const char *,const char *,const char *,uint64_t);\n'
        for name,body in stubs.items():
            p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
        source=work/'test.c'
        source.write_text(HARNESS[:HARNESS.index('int main(void)')]+(ROOT/'tools/tests/driver_protocol_host.c').read_text())
        cls.binaries={}
        for profile,backend in [('host','driver_class.c'),('mcu','driver_class_mcu.c')]:
            binary=work/profile;cls.binaries[profile]=binary
            subprocess.run(['cc','-std=gnu11','-pthread','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
                *(['-DCONFIG_MCU'] if profile=='mcu' else []),
                '-I'+str(work),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),str(source),
                str(ROOT/'kernel/drivers/core/driver_core.c'),str(ROOT/'kernel/drivers/core'/backend),
                str(ROOT/'kernel/drivers/core/driver_protocol.c'),str(ROOT/'kernel/firmware/node.c'),'-o',str(binary)],check=True)

    def workflow(self,name):
        for profile,binary in self.binaries.items():
            with self.subTest(profile=profile):
                subprocess.run([str(binary),name],check=True,timeout=15,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})

    def test_native_service_snapshot_and_rebind(self):self.workflow('native')
    def test_legacy_network_and_extension_share_one_endpoint(self):self.workflow('classes')
    def test_active_native_call_drains_before_instance_destruction(self):self.workflow('drain')
    def test_failed_publication_and_failed_probe_keep_correct_owners(self):self.workflow('failure')
