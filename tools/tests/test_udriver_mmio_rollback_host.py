#!/usr/bin/env python3
"""Compatibility entry for the shared physical mapper/PT rollback suite.

The user-driver adapter now delegates to the native MM transaction. The suite
executes the real Rust policy, C bridges and adapters, production PT unmap,
and runtime negative controls for failed-page/prefix/empty-path cleanup.
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.tests import test_physical_mapping

if __name__ == '__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(test_physical_mapping))
    if not result.wasSuccessful(): raise SystemExit(1)
    print('udriver-mmio-rollback: PASS (shared production mapper/unmap and runtime controls)')
