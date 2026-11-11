"""Real native mappings: nonidentity addresses, page pins and failure rollback."""
from pathlib import Path
import subprocess
import tempfile
import unittest

if __package__:
    from .test_firmware_model import ROOT, STUBS
else:
    from test_firmware_model import ROOT, STUBS


class DriverDmaMapping(unittest.TestCase):
    def test_mapping_ownership_and_errors(self):
        with tempfile.TemporaryDirectory(prefix="a20-dma-mapping-") as temp:
            work = Path(temp)
            stubs = {**STUBS,
                     "core/arch.h": "#define PAGE_OFFSET 0x100000000UL\n",
                     "drivers/core/driver_hwapi.h": '''#pragma once
#include "core/types.h"
struct device;
uint64_t dma_get_mask(const struct device *);
void dma_sync_for_device(void *, size_t);
void dma_sync_for_cpu(void *, size_t);
'''}
            for name, content in stubs.items():
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = work / "test"
            subprocess.run([
                "cc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(work), "-I" + str(ROOT / "kernel/include"),
                "-I" + str(ROOT / "kernel"),
                str(ROOT / "tools/tests/driver_dma_mapping_host.c"),
                str(ROOT / "kernel/drivers/core/dma_mapping.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
