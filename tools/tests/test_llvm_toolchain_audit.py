import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('llvm_check', Path(__file__).parents[1] / 'check-llvm.py')
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)

class ToolchainAuditTests(unittest.TestCase):
    def test_rejects_absolute_and_cross_gnu_tools(self):
        trace = '\n'.join(f'execve("{p}", [], []) = 0' for p in ['/usr/bin/gcc', '/usr/bin/riscv64-linux-gnu-ld', '/usr/lib/gcc/cc1', '/usr/bin/ar', '/usr/bin/ld.bfd', '/usr/bin/riscv64-unknown-elf-objdump'])
        self.assertEqual(len(audit.forbidden_tools(trace)), 6)

    def test_allows_llvm_rust_and_make(self):
        trace = '\n'.join(f'execve("/tools/{p}", [], []) = 0' for p in ['clang', 'clang++', 'ld.lld', 'lld-link', 'llvm-ar', 'llvm-nm', 'llvm-ranlib', 'llvm-objdump', 'clang-cpp', 'rust-lld', 'make', 'gmake', 'cmake'])
        self.assertEqual(audit.forbidden_tools(trace), set())

if __name__ == '__main__':
    unittest.main()
