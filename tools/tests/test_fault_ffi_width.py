"""Compile the actual fault bridge declarations as ILP32 and LP64 contracts.

The Rust modules use usize for virtual/physical addresses. A uint64_t C
declaration silently shifts subsequent output-pointer arguments on ARM EABI.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = '''
typedef __UINTPTR_TYPE__ uintptr_t;
typedef unsigned long long uint64_t;
typedef unsigned int pfn_t;
typedef unsigned char uint8_t;
typedef struct task task_t;
typedef struct mm mm_struct_t;
typedef struct cache page_cache_page_t;
'''
CONTRACTS = {
    'a20_cow_fault_locked_rust': 'int (*)(task_t*,mm_struct_t*,uintptr_t,pfn_t*,page_cache_page_t**)',
    'a20_cow_from_status_rust': 'int (*)(task_t*,mm_struct_t*,uintptr_t,pfn_t*)',
    'a20_fault_from_status_rust': 'int (*)(task_t*,mm_struct_t*,uintptr_t,uintptr_t,int)',
    'a20_cowf_status_open': 'int (*)(void*,mm_struct_t*,uintptr_t,uint8_t*,uintptr_t*,int*)',
    'a20_cowf_status_replace': 'int (*)(void*,uintptr_t,uintptr_t,uintptr_t,uint8_t,uintptr_t*)',
    'a20_fault_status_open': 'int (*)(void*,mm_struct_t*,uintptr_t,uint8_t*,int*,int*)',
    'a20_fault_status_map_if_absent': 'int (*)(void*,uintptr_t,uintptr_t,uintptr_t,uint8_t)',
    'a20_fault_status_flush_local': 'void (*)(uintptr_t)',
}


def source_contract():
    production = (ROOT / 'kernel/mm/fault.c').read_text()
    source = PRELUDE
    for index, (name, expected) in enumerate(CONTRACTS.items()):
        match = re.search(r'(?:extern\s+)?(?:int|void)\s+' + name + r'\s*\([^)]*\)', production)
        if not match:
            raise AssertionError('missing production declaration: ' + name)
        source += match.group() + ';\n'
        source += 'typedef ' + expected.replace('(*)', f'(*expected_{index})') + ';\n'
        source += f'_Static_assert(__builtin_types_compatible_p(__typeof__(&{name}), expected_{index}), "{name}: address width differs from Rust usize");\n'
    return source


class FaultFfiWidthTest(unittest.TestCase):
    def test_production_bridges_match_native_pointer_width(self):
        with tempfile.TemporaryDirectory(prefix='a20-fault-ffi-') as directory:
            path = Path(directory) / 'contract.c'
            path.write_text(source_contract())
            for model in ('-m32', '-m64'):
                subprocess.run(['cc', model, '-std=gnu11', '-nostdinc', '-fsyntax-only', str(path)], check=True)

    def test_old_wide_address_declaration_fails_ilp32(self):
        broken = source_contract().replace('uintptr_t stval,', 'uint64_t stval,', 1)
        self.assertNotEqual(broken, source_contract())
        with tempfile.TemporaryDirectory(prefix='a20-fault-ffi-negative-') as directory:
            path = Path(directory) / 'contract.c'
            path.write_text(broken)
            result = subprocess.run(['cc', '-m32', '-std=gnu11', '-nostdinc', '-fsyntax-only', str(path)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'a20_cow_fault_locked_rust', result.stderr)


if __name__ == '__main__':
    unittest.main()
