#!/usr/bin/env python3
"""Execute the shared production SBI dispatcher for both guest XLENs.

Hardware CSR, memory and lifecycle dependencies are modeled; actual ECALL
entry and cross-hart execution are separately checked by the QEMU smoke.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SHIMS = {
    'core/lock.h': '#pragma once\ntypedef struct { unsigned state; } spinlock_t;\n',
    'mm/pt.h': '''#pragma once
#include "core/types.h"
#include "core/errno.h"
typedef uint32_t pfn_t;
typedef struct { unsigned errors; } mm_pt_audit_report_t;
#define PAGE_SIZE 4096u
#define PTE_V 1u
#define PTE_R 2u
#define PTE_W 4u
#define PTE_X 8u
''',
    'mm/frame.h': '''#pragma once
#include "mm/pt.h"
void *pfn_to_virt(pfn_t);
void frame_put(pfn_t);
''',
}

class HypervisorSbiTest(unittest.TestCase):
    def test_dispatcher(self):
        for bits in (32, 64):
            with self.subTest(bits=bits), tempfile.TemporaryDirectory(prefix='a20-sbi-') as tmp:
                tmp = Path(tmp)
                for name, content in SHIMS.items():
                    path = tmp / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(content)
                binary = tmp / 'test'
                result = subprocess.run([
                    'cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    f'-DCONFIG_{bits}BIT=1', '-I', str(tmp),
                    '-I', str(ROOT / 'kernel/include'),
                    str(ROOT / 'kernel/arch/common/riscv/hyp_sbi.c'),
                    str(ROOT / 'tools/tests/hyp_sbi_host.c'), '-o', str(binary),
                ], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
