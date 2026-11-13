"""Instance argv must survive real make serialization without evaluation."""
from pathlib import Path
import json
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/cli'))
from a20_instance import parse_instance  # noqa: E402
from a20_test import _qemu_cmdline  # noqa: E402


class QemuInstanceArguments(unittest.TestCase):
    def query(self, folder, args):
        path = Path(folder) / 'instance.toml'
        path.write_text('arch = "x86_64"\n[machine]\nextra_qemu = ' + json.dumps(args) + '\n')
        return _qemu_cmdline(parse_instance(path))

    def test_kernel_command_line_is_one_argument(self):
        with tempfile.TemporaryDirectory(prefix='a20-qemu-args-') as folder:
            boot = 'driver.runtime_test=1 driver.amdvi_requester_test=1'
            argv = self.query(folder, ['-append', boot])
            self.assertEqual(argv[argv.index('-append') + 1], boot)
            self.assertNotIn('driver.amdvi_requester_test=1', argv)

    def test_quotes_dollars_and_substitutions_are_literal_data(self):
        with tempfile.TemporaryDirectory(prefix='a20-qemu-args-') as folder:
            sentinel = Path(folder) / 'unexpected-evaluation'
            literal = ('a "double" and \'single\' $HOME ${HOME} $$ '
                       f'$(shell touch {sentinel}) $(touch {sentinel}) `touch {sentinel}`')
            argv = self.query(folder, ['-fw_cfg', literal, '-name', ''])
            self.assertEqual(argv[argv.index('-fw_cfg') + 1], literal)
            self.assertEqual(argv[argv.index('-name') + 1], '')
            self.assertFalse(sentinel.exists(), 'make/query shell evaluated literal argv')


if __name__ == '__main__':
    unittest.main()
