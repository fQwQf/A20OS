"""Bulk-console manifest validation and actual Makefile/QEMU argument wiring."""
import shlex
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from a20_instance import parse_instance
from a20_derive import derive_make_vars
from a20_validate import validate_instance
from a20_make import query_make


class ConsoleConfigTests(unittest.TestCase):
    def parse(self, text):
        directory=tempfile.TemporaryDirectory();self.addCleanup(directory.cleanup)
        path=Path(directory.name)/'console-test.toml';path.write_text(text)
        return parse_instance(path)

    def test_existing_serial_default(self):
        inst=self.parse('arch="x86_64"\n')
        self.assertIsNone(inst.machine.console)
        self.assertFalse(any(v.startswith('QEMU_CONSOLE=') for v in derive_make_vars(inst)))

    def test_default_cli_has_one_merged_append_and_two_console_transports(self):
        inst=parse_instance(ROOT/'instances/cli-x86_64.toml')
        self.assertEqual(inst.machine.console,'virtio')
        self.assertFalse(validate_instance(inst,ROOT))
        values=query_make(inst,['QEMU_FLAGS_NO_SDCARD','QEMU_CONSOLE_RUNNER'],['NET_GUEST_IP=10.0.3.2'])
        flags=shlex.split(values['QEMU_FLAGS_NO_SDCARD'])
        self.assertEqual(flags.count('-append'),1)
        append=flags[flags.index('-append')+1]
        self.assertIn('a20.console=vport0',append)
        self.assertIn('a20.ip=10.0.3.2',append)
        self.assertNotIn('-nographic',flags)
        self.assertIn('tools/qemu_console.py',values['QEMU_CONSOLE_RUNNER'])
        from qemu_console import console_argv
        launch=console_argv(['qemu-system-x86_64',*flags],'/tmp/test-console')
        self.assertIn('chardev:a20uart',launch)
        self.assertIn('virtserialport,chardev=a20terminal,bus=a20tty.0,nr=1',launch)
        self.assertFalse(any('mux=on' in value for value in launch))

    def test_reject_invalid_console_and_unsupported_combinations(self):
        for console,arch,extra in [('bogus','x86_64',''),('virtio','riscv64',''),
                                   ('virtio','x86_64','\n[gui]\nenabled=true\n'),
                                   ('virtio','x86_64','\n[kernel]\ndriver_deployment="embedded"\n')]:
            with self.subTest(console=console,arch=arch,extra=extra):
                inst=self.parse(f'arch="{arch}"\n[machine]\nconsole="{console}"\n[rootfs]\nworld="cli"\n'+extra)
                errors=validate_instance(inst,ROOT)
                self.assertTrue(any('machine.console' in error for error in errors), errors)


if __name__=='__main__':
    unittest.main()
