"""Host fixture for negotiated VirtIO block queue geometry.

The fixture brace-extracts the production queue-size and legacy-avail-offset
helpers from the current repository kernel source.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/drivers/block/virtio_blk.c"


def extract(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected one production definition: {signature}")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated production function: {signature}")


def run_c(source: str) -> None:
    with tempfile.TemporaryDirectory(prefix="a20-virtio-queue-layout-") as tmp:
        c_path = Path(tmp) / "layout.c"
        exe_path = Path(tmp) / "layout"
        c_path.write_text(source)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-O2", str(c_path), "-o", str(exe_path)], check=True)
        subprocess.run([str(exe_path)], check=True)


def main() -> None:
    production = SOURCE.read_text()
    choose = extract(production, "static uint16_t virtio_blk_choose_queue_size(")
    offset = extract(production, "static size_t virtio_blk_legacy_avail_offset(")
    run_c(r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define PAGE_SIZE 4096U
#define VIRTIO_QUEUE_SIZE 32U
#define VIRTIO_QUEUE_MAX_SIZE 64U
typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } virtq_desc_t;
typedef struct { uint16_t flags, idx; uint16_t ring[64]; uint16_t used_event; } virtq_avail_t;
""" + choose + offset + r"""
int main(void) {
    const struct { uint32_t qmax; uint16_t depth; uint16_t slots; } cases[] = {
        {0, 0, 0}, {31, 0, 0}, {32, 32, 10}, {48, 32, 10},
        {63, 32, 10}, {64, 64, 21}, {65, 64, 21}, {127, 64, 21},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint16_t actual = virtio_blk_choose_queue_size(cases[i].qmax);
        if (actual != cases[i].depth || actual / 3U != cases[i].slots)
            return 1;
        if (actual) {
            size_t avail = virtio_blk_legacy_avail_offset(actual);
            if (avail != (size_t)actual * sizeof(virtq_desc_t) ||
                avail + sizeof(virtq_avail_t) > PAGE_SIZE)
                return 2;
        }
    }
    return 0;
}
""")


if __name__ == "__main__":
    main()
