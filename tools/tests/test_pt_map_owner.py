"""Differential host fixture for the real pt_walk owner and pt_map_cls code."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected one definition of {signature!r}")
    start = source.index(signature)
    brace = source.find("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError("unterminated C function")


class PtMapOwner(unittest.TestCase):
    def test_real_walk_and_status_map_reuse(self):
        source = (ROOT / "kernel/mm/mm.c").read_text()
        helper = extract_function(source, "static pte_t *pt_walk_with_owner(")
        wrapper = extract_function(source, "pte_t *pt_walk(")
        leaf_install = extract_function(source, "static void pt_install_leaf_cls(")
        mapper = extract_function(source, "int pt_map_cls(")
        fixture = (ROOT / "tools/tests/pt_map_owner_host.c").read_text()
        fixture = fixture.replace("/* WALK_HELPER */", helper)
        fixture = fixture.replace("/* PT_WALK_WRAPPER */", wrapper)
        fixture = fixture.replace("/* PT_INSTALL_LEAF */", leaf_install)
        fixture = fixture.replace("/* PT_MAP_CLS */", mapper)

        with tempfile.TemporaryDirectory(prefix="a20-pt-map-owner-") as tmp:
            c_file = Path(tmp) / "pt_map_owner.c"
            exe = Path(tmp) / "pt_map_owner"
            c_file.write_text(fixture)
            for flags in ([], ["-DCONFIG_SWAP=1"]):
                subprocess.run(
                    ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-fsanitize=undefined", *flags, str(c_file), "-o", str(exe)],
                    check=True,
                    cwd=ROOT,
                )
                subprocess.run([str(exe)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
