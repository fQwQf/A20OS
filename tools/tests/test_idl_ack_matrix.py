"""Exercise the public ACK gate from a checkout with no private .git helpers."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class AckMatrixIntegrationTests(unittest.TestCase):
    def run_matrix(self, arch, build_failure=False):
        with tempfile.TemporaryDirectory(prefix="a20-idl-matrix-") as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            scripts = root / "tools/xlator"
            scripts.mkdir(parents=True)
            shutil.copyfile(ROOT / "tools/xlator/run-idl-ack-matrix.sh",
                            scripts / "run-idl-ack-matrix.sh")
            (scripts / "run-idl-ack-guest.py").write_text('''
import argparse
from pathlib import Path
p = argparse.ArgumentParser()
for flag in ('arch', 'kernel', 'image', 'output', 'timeout'):
    p.add_argument('--' + flag)
a = p.parse_args()
assert Path(a.kernel).is_file()
assert Path(a.image).read_bytes() == b'base\\ninjected\\ninjected\\n'
Path(a.output, 'ran-guest').write_text(a.arch)
''')
            bindir = root / "bin"
            bindir.mkdir()
            make = bindir / "make"
            make.write_text('''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with Path('make-calls.jsonl').open('a') as log:
    log.write(json.dumps(args) + '\\n')
if os.environ.get('FAIL_BUILD') == '1':
    sys.exit(42)
arch = next(a.split('=', 1)[1] for a in args if a.startswith('ARCH='))
bdir = Path('kernel-build')
if 'print-build-dir' in args:
    print(bdir)
    sys.exit(0)
bdir.mkdir(exist_ok=True)
(bdir / ('kernel.bin' if arch == 'aarch64' else 'kernel.elf')).touch()
if 'dev-build' in args:
    (bdir / 'fat32.img').write_bytes(b'base\\n')
else:
    udir = Path(next(a.split('=', 1)[1] for a in args if a.startswith('USER_BUILD_DIR=')))
    udir.mkdir(parents=True, exist_ok=True)
    tag = 'rv' if arch == 'riscv64' else arch
    for binary in ('rust-idl-peer-ack-', 'rust-idl-adversary-'):
        (udir / (binary + tag)).touch()
''')
            make.chmod(0o755)
            mcopy = bindir / "mcopy"
            mcopy.write_text('''#!/usr/bin/env python3
from pathlib import Path
import sys
args = sys.argv[1:]
assert Path(args[-2]).is_file()
with Path(args[args.index('-i') + 1]).open('ab') as image:
    image.write(b'injected\\n')
''')
            mcopy.chmod(0o755)
            env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ['PATH'])
            env.pop('IDL_ACK_USER_BUILD_DIR', None)
            env.pop('IDL_ACK_OUTPUT_DIR', None)
            env['FAIL_BUILD'] = str(int(build_failure))
            result = subprocess.run(['sh', str(scripts / 'run-idl-ack-matrix.sh'), arch],
                                    cwd=root, env=env, capture_output=True, text=True,
                                    timeout=15)
            if build_failure:
                self.assertEqual(result.returncode, 42, result.stderr)
                self.assertFalse((root / f'build/idl-ack-{arch}/ran-guest').exists())
                return
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((root / 'kernel-build/fat32.img').read_bytes(), b'base\n')
            self.assertEqual((root / f'build/idl-ack-{arch}/ran-guest').read_text(), arch)
            calls = [json.loads(line) for line in (root / 'make-calls.jsonl').read_text().splitlines()]
            self.assertIn('dev-build', calls[0])
            self.assertTrue(all('CONFIG_DEBUG_SHUTDOWN_AUDIT=1' in call for call in calls))
            target = 'native-rust-idl-ack-test' + ('-aarch64' if arch == 'aarch64' else '')
            self.assertIn(target, calls[1])
            if arch == 'riscv64':
                self.assertTrue(all('NR_CPUS=2' in call for call in calls))

    def test_aarch64_gate_builds_and_runs_without_private_helpers(self):
        self.run_matrix('aarch64')

    def test_riscv_gate_preserves_smp2_and_base_image(self):
        self.run_matrix('riscv64')

    def test_build_failure_prevents_guest_execution(self):
        self.run_matrix('aarch64', build_failure=True)


if __name__ == '__main__':
    unittest.main()
