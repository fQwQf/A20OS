"""World selectors must not leak foreign-architecture package names into apk."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('mkrootfs', Path(__file__).resolve().parents[1] / 'mkrootfs.py')
mkrootfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkrootfs)


class WorldArchitectures(unittest.TestCase):
    def parse(self, text, arch=None):
        with tempfile.TemporaryDirectory() as directory:
            world = Path(directory) / 'desktop.world'
            world.write_text(text)
            return mkrootfs.read_world([world], arch)

    def test_plain_specs_versions_and_repository_tags_are_preserved(self):
        self.assertEqual(self.parse('firefox\nvim>=9.1 # comment\ngimp@testing\n'),
                         ['firefox', 'vim>=9.1', 'gimp@testing'])

    def test_architecture_selects_only_available_packages(self):
        text = 'firefox\n[x86_64,aarch64] libreoffice-writer\n[riscv64] riscv-helper\n'
        self.assertEqual(self.parse(text, 'x86_64'), ['firefox', 'libreoffice-writer'])
        self.assertEqual(self.parse(text, 'aarch64'), ['firefox', 'libreoffice-writer'])
        self.assertEqual(self.parse(text, 'riscv64'), ['firefox', 'riscv-helper'])
        self.assertEqual(self.parse(text, 'loongarch64'), ['firefox'])

    def test_invalid_or_ambiguous_selectors_fail_before_installation(self):
        for text, arch in [('[x86_64] gimp', None), ('[typo] gimp', 'x86_64'),
                           ('[] gimp', 'x86_64'), ('[x86_64] ', 'x86_64'),
                           ('[x86_64,] gimp', 'x86_64')]:
            with self.subTest(text=text), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    self.parse('firefox\n' + text, arch)


if __name__ == '__main__':
    unittest.main()
