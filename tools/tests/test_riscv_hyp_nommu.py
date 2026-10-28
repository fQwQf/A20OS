#!/usr/bin/env python3
"""Link production RISC-V stage-2 absence queries with the unavailable backend.

These host tests verify the NoMMU contract, not RISC-V guest execution. Public
HYP structures are real; only unrelated MM/lock/log headers are shimmed.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SHIMS = {
    "mm/pt.h": '''#pragma once
#include "core/types.h"
typedef uint32_t pfn_t;
typedef struct { unsigned errors; } mm_pt_audit_report_t;
#define PAGE_SIZE 4096u
''',
    "core/lock.h": '''#pragma once
#include "core/types.h"
typedef struct { unsigned state; } spinlock_t;
''',
    "mm/frame.h": "#pragma once\n",
    "mm/mm.h": "#pragma once\n",
    "core/stdio.h": "#pragma once\n",
}
HARNESS = r'''
#include "hyp/hyp_vcpu.h"
#include "core/errno.h"
#include <assert.h>
#include <string.h>
extern pte_t hyp_riscv_s2_prot(hyp_vm_t *, uint64_t);
extern int hyp_riscv_s2_pin_access(hyp_vm_t *, uint64_t, pte_t, pfn_t *);
int main(void) {
    hyp_vm_t vm, before;
    memset(&vm, 0xa5, sizeof(vm));
    memcpy(&before, &vm, sizeof(vm));
    assert(hyp_arch_backend() == NULL);
    assert(hyp_riscv_s2_prot(NULL, 0) == 0);
    assert(hyp_riscv_s2_prot(&vm, 0x80000000) == 0);
    assert(hyp_riscv_s2_prot(&vm, ~(uint64_t)0) == 0);
    pfn_t pinned = 123;
    assert(hyp_riscv_s2_pin_access(&vm, 0x80000000, 0, &pinned) == -EOPNOTSUPP);
    assert(pinned == 123);
    assert(memcmp(&vm, &before, sizeof(vm)) == 0);
    return 0;
}
'''


class RiscvNoMmuHypervisor(unittest.TestCase):
    def test_absent_stage2_grants_no_permissions_and_registers_no_backend(self):
        for arch, bits in (("riscv64", 64), ("riscv32", 32)):
            # Also cover the same source when host page-table support is absent.
            for flags in (("-DCONFIG_NOMMU", "-DARCH_HAS_PGTABLE_OPS"), ()):
                with self.subTest(arch=arch, flags=flags), tempfile.TemporaryDirectory() as directory:
                    temp = Path(directory)
                    for name, content in SHIMS.items():
                        path = temp / name
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text(content)
                    source = temp / "main.c"
                    source.write_text(HARNESS)
                    binary = temp / "test"
                    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                    "-fno-builtin", f"-DCONFIG_{bits}BIT", *flags,
                                    "-I", str(temp), "-I", str(ROOT / "kernel/include"),
                                    str(ROOT / f"kernel/arch/{arch}/hyp/hyp_s2.c"),
                                    str(ROOT / "kernel/hyp/hyp_backend.c"), str(source),
                                    "-o", str(binary)], check=True, capture_output=True, text=True)
                    subprocess.run([str(binary)], check=True, capture_output=True, text=True)

if __name__ == "__main__":
    unittest.main()
