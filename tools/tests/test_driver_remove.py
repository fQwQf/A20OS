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
        prelude = prelude.replace('void *kmalloc(size_t n) { return malloc(n); }',
                                  'static int probe_allocations_blocked;\nvoid *kmalloc(size_t n) { return probe_allocations_blocked ? NULL : malloc(n); }')
        prelude = prelude.replace('return calloc(n, size);', 'return probe_allocations_blocked ? NULL : calloc(n, size);')
        prelude = prelude.replace('return realloc(p, n);', 'return probe_allocations_blocked ? NULL : realloc(p, n);')
        cases = (ROOT / 'tools/tests/driver_remove_host.c').read_text()
        marker = 'int main(int argc,char **argv) {'
        cases = cases.replace(marker, (ROOT / 'tools/tests/driver_probe_cleanup_host.c').read_text() + '\n' + (ROOT / 'tools/tests/driver_remove_prepare_host.c').read_text() + '\n' + marker)
        cases = cases.replace('if(!strcmp(argv[1],"arguments"))',
                              'if(!strncmp(argv[1],"probe-",6))probe_cleanup_case(argv[1]+6);\n else if(!strncmp(argv[1],"prepare-",8))prepare_case(argv[1]+8);\n else if(!strcmp(argv[1],"arguments"))')
        fixture.write_text(prelude + cases)
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
                    str(ROOT / 'kernel/drivers/core/driver_class.c'), str(ROOT / 'kernel/drivers/core/driver_protocol.c'), str(ROOT / 'kernel/firmware/node.c'),
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


    def test_prepare_checkpoints_precede_children_and_survive_each_later_failure(self):
        self.run_cases('prepare-order')

    def test_prepare_failure_during_probe_preserves_original_error_and_graph(self):
        self.run_cases('prepare-probe-error', 'prepare-early')

    def test_prepare_oom_uses_partial_remove_without_arming_an_action(self):
        self.run_cases('prepare-oom')

    def test_probe_error_uses_explicit_partial_cleanup(self):
        self.run_cases('probe-early', 'probe-positive', 'probe-deferred')

    def test_probe_error_retains_context_and_original_error_until_retry(self):
        self.run_cases('probe-held', 'probe-deferred-held')

    def test_probe_cleanup_waits_for_failed_child_before_quiescing_parent(self):
        self.run_cases('probe-child-held')

    def test_probe_cleanup_is_allocation_free_and_success_is_unchanged(self):
        self.run_cases('probe-success')

    def test_nested_probe_restores_cleanup_ownership(self):
        self.run_cases('probe-nested')

    def test_probe_cleanup_rejects_missing_remove_and_unpublished_devices(self):
        self.run_cases('probe-missing-remove', 'probe-unpublished')

    def test_legacy_failed_probe_keeps_its_original_rollback_contract(self):
        self.run_cases('probe-legacy')


if __name__ == '__main__':
    unittest.main()
