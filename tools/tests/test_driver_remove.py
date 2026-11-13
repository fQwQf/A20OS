"""Fallible removal using production core/class/I2C with C and Rust registries."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_firmware_model import ROOT, STUBS


class DriverRemove(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-driver-remove-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        stubs = {**STUBS, 'net/socket/socket_internal.h':
                 '#include "core/types.h"\nvoid netlink_uevent_emit(const char *,const char *,const char *,uint64_t);\n'}
        for name, content in stubs.items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        prelude = (ROOT / 'tools/tests/driver_quiesce_host.c').read_text().split('static unsigned quiesced,')[0]
        fixture = work / 'fixture.c'
        fixture.write_text(prelude + (ROOT / 'tools/tests/driver_remove_host.c').read_text())
        wrapper = work / 'registry.rs'
        wrapper.write_text('#![no_std]\n#![allow(unused_attributes)]\n#[path = ' + repr(str(ROOT / 'kernel/drivers/core/registry_state.rs')).replace("'", '"') + '] mod registry;\n'
                           '#[panic_handler] fn panic(_: &core::panic::PanicInfo)->! { unsafe extern "C" {fn abort()->!;} unsafe{abort()} }\n')
        library = work / 'registry.a'
        subprocess.run(['rustc', '--edition=2021', '--crate-type=staticlib', '-Cpanic=abort', '-Copt-level=1',
                        str(wrapper), '-o', str(library)], check=True)
        cls.binaries = []
        for implementation in ('c', 'rust'):
            binary = work / implementation
            args = ['cc', '-std=gnu99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-DCONFIG_NR_CPUS=2', '-I'+str(work), '-I'+str(ROOT / 'kernel/include'), '-I'+str(ROOT / 'kernel'),
                    str(fixture), str(ROOT / 'kernel/drivers/core/driver_core.c'),
                    str(ROOT / 'kernel/drivers/core/driver_class.c'), str(ROOT / 'kernel/firmware/node.c'),
                    str(ROOT / 'kernel/drivers/i2c/i2c-core.c'), '-o', str(binary)]
            if implementation == 'rust':
                args += ['-DCONFIG_DRIVER_REGISTRY_RUST', str(library)]
            subprocess.run(args, check=True)
            cls.binaries.append(binary)

    def run_cases(self, *cases):
        for binary in self.binaries:
            for case in cases:
                with self.subTest(registry=binary.name, case=case):
                    subprocess.run([str(binary), case], check=True, timeout=10)

    def test_device_and_driver_failure_keep_binding_resources_and_pins(self):
        self.run_cases('device', 'driver')

    def test_multi_device_unregister_retains_driver_after_partial_progress(self):
        self.run_cases('multi')

    def test_consumer_and_parent_failures_preserve_providers(self):
        self.run_cases('dependency', 'parent', 'provider')

    def test_publication_rollback_failure_cannot_fall_through_to_another_driver(self):
        self.run_cases('publication')

    def test_positive_remove_result_is_failure_and_hotplug_event_is_suppressed(self):
        self.run_cases('positive')

    def test_real_i2c_reference_blocks_remove_until_put(self):
        self.run_cases('i2c')

    def test_invalid_and_absent_objects(self):
        self.run_cases('arguments')

    def test_hotplug_notification_retains_metadata_through_last_registry_put(self):
        self.run_cases('last-reference')

    def test_withdrawal_suppresses_new_bindings_and_rejects_recursive_teardown(self):
        self.run_cases('withdrawal', 'parent-publication')


if __name__ == '__main__':
    unittest.main()
