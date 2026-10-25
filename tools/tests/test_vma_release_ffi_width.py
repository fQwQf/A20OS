"""The final-destroy run release bridge must preserve Rust's usize ABI.

On ARM EABI a uint64_t address consumes an aligned register pair, moving the
PFN-array pointer and count to different argument locations. This production
prototype check catches the defect without needing a full kernel boot.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NAME = 'a20_vma_free_run_release_rust'


def contract_source():
    production = (ROOT / 'kernel/mm/vma.c').read_text()
    match = re.search(r'extern\s+void\s+' + NAME + r'\s*\([^)]*\)', production)
    if not match:
        raise AssertionError('missing production VMA release declaration')
    return '''
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __SIZE_TYPE__ size_t;
typedef unsigned long long uint64_t;
typedef unsigned int pfn_t;
typedef struct segment mm_seg_t;
''' + match.group() + ''';
typedef void (*expected_release)(mm_seg_t *, uintptr_t, const pfn_t *, size_t, int);
_Static_assert(__builtin_types_compatible_p(__typeof__(&a20_vma_free_run_release_rust), expected_release),
               "VMA run release argument widths differ from Rust usize");
'''


def compile_contract(source, model):
    with tempfile.TemporaryDirectory(prefix='a20-vma-release-ffi-') as directory:
        path = Path(directory) / 'contract.c'
        path.write_text(source)
        return subprocess.run(['cc', model, '-std=gnu11', '-nostdinc',
                               '-fsyntax-only', str(path)], capture_output=True, text=True)


class VmaReleaseFfiWidthTest(unittest.TestCase):
    def test_production_release_matches_native_address_and_count(self):
        for model in ('-m32', '-m64'):
            with self.subTest(model=model):
                result = compile_contract(contract_source(), model)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_old_wide_address_fails_ilp32(self):
        source = contract_source()
        broken = source.replace('uintptr_t start_va,', 'uint64_t start_va,', 1)
        self.assertNotEqual(source, broken)
        result = compile_contract(broken, '-m32')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('VMA run release argument widths', result.stderr)

    def test_old_int_count_fails_lp64(self):
        source = contract_source()
        broken = source.replace('size_t count,', 'int count,', 1)
        self.assertNotEqual(source, broken)
        result = compile_contract(broken, '-m64')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('VMA run release argument widths', result.stderr)


if __name__ == '__main__':
    unittest.main()
