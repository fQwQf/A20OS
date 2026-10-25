"""Host checks for VirtIO block's runtime queue-depth selection."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "kernel/drivers/block/virtio_blk.c"
HEADER = ROOT / "kernel/include/drivers/block/virtio_blk.h"


def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected one definition of {signature!r}")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError(f"unterminated function {signature!r}")


class VirtioBlkCapacity(unittest.TestCase):
    def test_negotiated_depth_chooses_supported_power_of_two(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("cc is required for the host oracle")
        helper = extract_function(
            DRIVER.read_text(), "static uint16_t virtio_blk_choose_queue_size("
        )
        harness = r"""
#include <stdint.h>
#include <stdio.h>
#define VIRTIO_QUEUE_SIZE 32
#define VIRTIO_QUEUE_MAX_SIZE 64
""" + helper + r"""
int main(void) {
    const struct { uint32_t advertised; uint16_t expected; } cases[] = {
        {0, 0}, {31, 0}, {32, 32}, {48, 32}, {63, 32},
        {64, 64}, {65, 64}, {127, 64},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint16_t actual = virtio_blk_choose_queue_size(cases[i].advertised);
        if (actual != cases[i].expected) {
            fprintf(stderr, "qmax=%u expected=%u got=%u\n",
                    cases[i].advertised, cases[i].expected, actual);
            return 1;
        }
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="a20-virtio-blk-depth-") as tmp:
            source = Path(tmp) / "depth.c"
            executable = Path(tmp) / "depth"
            source.write_text(harness)
            subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-O2", str(source), "-o", str(executable)],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    def test_legacy_available_ring_follows_actual_descriptor_depth(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("cc is required for the host oracle")
        helper = extract_function(
            DRIVER.read_text(), "static size_t virtio_blk_legacy_avail_offset("
        )
        harness = r"""
#include <stddef.h>
#include <stdint.h>
#define PAGE_SIZE 4096U
typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } virtq_desc_t;
typedef struct { uint16_t flags, idx; uint16_t ring[64]; uint16_t used_event; } virtq_avail_t;
""" + helper + r"""
int main(void) {
    const uint16_t depths[] = {32, 64};
    for (unsigned i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i) {
        size_t offset = virtio_blk_legacy_avail_offset(depths[i]);
        if (offset != (size_t)depths[i] * 16U ||
            offset + sizeof(virtq_avail_t) > PAGE_SIZE)
            return 1;
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="a20-virtio-blk-legacy-layout-") as tmp:
            source = Path(tmp) / "legacy-layout.c"
            executable = Path(tmp) / "legacy-layout"
            source.write_text(harness)
            subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-O2", str(source), "-o", str(executable)],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)

    def test_storage_and_runtime_depth_are_separate(self):
        header = HEADER.read_text()
        driver = DRIVER.read_text()
        self.assertIn("#define VIRTIO_QUEUE_SIZE        32", header)
        self.assertIn("#define VIRTIO_QUEUE_MAX_SIZE    64", header)
        self.assertIn("ring[VIRTIO_QUEUE_MAX_SIZE]", header)
        self.assertIn("vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, queue_size);", driver)
        self.assertIn("a20_virtio_blk_queue_init(&inst->queue_state, queue_size,", driver)
        self.assertIn("inst->queue_size != queue_size", driver)


if __name__ == "__main__":
    unittest.main()
