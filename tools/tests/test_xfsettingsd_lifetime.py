"""Real GIO bus-disconnect regression for the patched upstream GTK helper."""
from pathlib import Path
import importlib.util
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('xfsettingsd_fix', ROOT / 'tools/packaging/xfsettingsd_fix.py')
fix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fix)


def function(source, name):
    found = re.search(r'^static [^;{}]*\b' + name + r'\s*\([^;{}]*\)\s*\{.*?^\}',
                      source, re.M | re.S)
    if not found:
        raise AssertionError('missing production function ' + name)
    return found.group()


class XfsettingsdLifetime(unittest.TestCase):
    def test_real_bus_loss_does_not_consume_main_reference(self):
        archive = ROOT / 'build/downloads/xfce4-settings-4.20.1.tar.bz2'
        if not archive.exists() or not all(shutil.which(x) for x in ('pkg-config', 'gdbus-codegen', 'dbus-daemon', 'cc')):
            self.skipTest('needs verified source cache, compiler and GIO private-bus tools')
        if subprocess.run(['pkg-config', '--exists', 'gio-2.0']).returncode:
            self.skipTest('needs GIO development headers')
        fix.source_archive()  # Verify the fixed upstream SHA512, never test a guessed source.
        evidence = Path(os.environ['A20_XFCE_LIFETIME_EVIDENCE']) if 'A20_XFCE_LIFETIME_EVIDENCE' in os.environ else None
        if evidence:
            evidence.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='a20-xfsettingsd-ref-') as directory:
            work = Path(directory)
            with tarfile.open(archive) as source_tar:
                prefix = 'xfce4-settings-4.20.1/xfsettingsd/'
                original = source_tar.extractfile(prefix + 'gtk-settings.c').read().decode()
                xml = source_tar.extractfile(prefix + 'gtk-settings-exported.xml').read()
            (work / 'gtk-settings-exported.xml').write_bytes(xml)
            subprocess.run(['gdbus-codegen', '--interface-prefix', 'org.gtk.Settings',
                '--c-namespace', 'XfceGtkSettingsHelper', '--generate-c-code',
                str(work / 'gtk-settings-exported'), str(work / 'gtk-settings-exported.xml')], check=True)
            target = work / 'xfsettingsd/gtk-settings.c'
            target.parent.mkdir()
            target.write_text(original)
            subprocess.run(['patch', '-p1', '-i', str(fix.PATCH)], cwd=work, check=True,
                           capture_output=True)
            versions = [('original', original, 2), ('patched', target.read_text(), 0)]
            flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gio-2.0'], text=True).split()
            for label, production, expected in versions:
                struct = re.search(r'struct _XfceGtkSettingsHelper\s*\{.*?\n\};', production, re.S).group()
                names = ['bus_acquired', 'name_lost', 'xfce_gtk_settings_helper_finalize']
                if 'settings_bus_data_free' in production:
                    names[2:2] = ['settings_bus_data_free', 'settings_bus_own']
                callbacks = '\n'.join(function(production, name) for name in names)
                init = function(production, 'xfce_gtk_settings_helper_init')
                registration = init[init.index('    /* synchronization via gtk-modules:'):
                                    init.index('    source = g_settings_schema_source_get_default ();')]
                generated = (ROOT / 'tools/tests/xfsettingsd_lifetime_host.c').read_text()
                generated = generated.replace('/* PRODUCTION_STRUCT */', struct)
                generated = generated.replace('/* PRODUCTION_CALLBACKS */', callbacks)
                generated = generated.replace('/* PRODUCTION_REGISTRATION */', registration)
                code, binary = work / (label + '.c'), work / label
                code.write_text(generated)
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function', '-I', str(work),
                    str(code), str(work / 'gtk-settings-exported.c'), *flags,
                    '-o', str(binary)], check=True, capture_output=True)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=8)
                if evidence:
                    (evidence / (label + '.log')).write_text(result.stdout + result.stderr)
                    (evidence / (label + '.c')).write_text(generated)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                self.assertIn('LIFETIME_BAD' if expected else 'LIFETIME_OK', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
