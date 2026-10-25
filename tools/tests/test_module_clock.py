"""Driver inline timeouts must link against the public clock ABI."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ModuleClockContract(unittest.TestCase):
    def test_module_frequency_uses_public_export_on_constant_and_runtime_arches(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'core').mkdir()
            (stage / 'core/types.h').write_text('#include <stdint.h>\n')
            (stage / 'core/consts.h').write_text('')
            header = ROOT / 'kernel/include/core/timer.h'
            for module in (False, True):
                for runtime in (False, True):
                    with self.subTest(module=module, runtime_frequency=runtime):
                        body = f'#include "{header}"\n#include <assert.h>\n'
                        if module:
                            # Deliberately provide no architecture-private API.
                            body += 'uint64_t clock_ticks_per_sec(void) { return 25000000; }\n'
                        elif runtime:
                            body += 'uint64_t arch_timer_frequency(void) { return 24000000; }\n'
                        expected = 50000 if module else 48000
                        body += f'int main(void) {{ assert(MS_TO_TICKS(2)=={expected}); return 0; }}\n'
                        source = stage / 'model.c'
                        source.write_text(body)
                        command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                   '-I', str(stage), f'-DARCH_TIMER_FREQ_RUNTIME={int(runtime)}',
                                   '-DARCH_TIMER_FREQ=24000000']
                        if module:
                            command.append('-DCONFIG_DRIVER_MODULE')
                        built = subprocess.run([*command, str(source), '-o', str(stage / 'model')],
                                               capture_output=True, text=True)
                        self.assertEqual(built.returncode, 0, built.stderr)
                        result = subprocess.run([str(stage / 'model')], capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
