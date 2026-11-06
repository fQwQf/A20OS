"""Exercise the real per-PTE cache-owner metadata helpers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(signature)


class PtCacheOwner(unittest.TestCase):
    def test_note_peek_replace_and_absent(self):
        source = (ROOT / "kernel/mm/pt.c").read_text()
        helpers = "\n\n".join(extract(source, signature) for signature in [
            "static inline uint8_t *cls_slot(",
            "static inline uint8_t *safe_bit(",
            "static struct page_cache_page **cache_owner_slot(",
            "struct page_cache_page *mm_pt_cache_peek(",
            "void mm_pt_cache_note(",
            "static void pt_note_present_meta(",
            "void mm_pt_note_present_meta(",
            "void mm_pt_note_present(",
            "void mm_pt_note_absent(",
            "int mm_pt_safe_set(",
            "int mm_pt_safe_clear(",
            "int mm_pt_refresh_leaf_prot(",
            "int mm_pt_sync_status_meta(",
            "void mm_pt_meta_set_stale(",
        ])
        fixture = (ROOT / "tools/tests/pt_cache_owner_host.c").read_text()
        fixture = fixture.replace("/* REAL_CACHE_HELPERS */", helpers)
        with tempfile.TemporaryDirectory(prefix="a20-pt-cache-owner-") as tmp:
            cfile = Path(tmp) / "owner.c"
            exe = Path(tmp) / "owner"
            cfile.write_text(fixture)
            subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined",
                            str(cfile), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
