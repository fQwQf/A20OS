"""Compile an adversarial C fixture around the production TLB hold drain."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract_block(source: str, start_marker: str, terminator: str) -> str:
    start = source.index(start_marker)
    end = source.index(terminator, start) + len(terminator)
    return source[start:end]


def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected exactly one production definition: {signature}")
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


class MMTlbHoldReleaseTests(unittest.TestCase):
    def test_production_drain_does_not_put_page_table_ownership_twice(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("cc is required for this host fixture")

        vm_source = (ROOT / "kernel/mm/vm.c").read_text()
        vm_header = (ROOT / "kernel/include/mm/vm.h").read_text()
        definitions = extract_block(vm_source, "enum {\n    MM_TLB_HOLD_FRAME", "};")
        hold_type = extract_block(vm_header, "typedef struct mm_tlb_hold {", "} mm_tlb_hold_t;")
        helper = extract_function(vm_source, "static void mm_tlb_release_holds(")
        fixture = (ROOT / "tools/tests/mmtlb_hold_release_host.c").read_text()
        fixture = fixture.replace(
            "/* INSERT_PRODUCTION_MM_TLB_HOLD_DEFINITIONS */",
            definitions + "\n" + hold_type + "\n" + extract_block(vm_source, "typedef struct mm_tlb_page_run {", "} mm_tlb_page_run_t;"),
        )
        fixture = fixture.replace(
            "/* INSERT_PRODUCTION_MM_TLB_RELEASE_HELPER */", helper
        )
        if "INSERT_PRODUCTION_" in fixture:
            self.fail("production source insertion point was not replaced")

        with tempfile.TemporaryDirectory(prefix="a20-mmtlb-hold-release-") as tmp:
            c_file = Path(tmp) / "hold_release.c"
            binary = Path(tmp) / "hold_release"
            c_file.write_text(fixture)
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                 str(c_file), "-o", str(binary)],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("production helper ownership and batching PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
