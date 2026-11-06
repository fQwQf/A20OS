"""Check real C lock-token adapters independently of the Rust transaction."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_cow_anon_child_batch import extract_block

ROOT = Path(__file__).resolve().parents[2]


class SnapshotToken(unittest.TestCase):
    def test_lock_and_reference_identity(self):
        text = (ROOT / "kernel/mm/cow.c").read_text()
        struct = text[text.index("struct a20_cow_shared_snapshot {"):text.index("uint64_t a20_cow_policy_flags(")]
        functions = text[text.index("static int a20_cow_share_snapshot_from_slot("):text.index("#ifndef CONFIG_NOMMU\n#if defined(ARCH_HAS_PGTABLE_OPS)\n/* The child is unpublished") ]
        functions = extract_block(text, "static int cow_file_leaf_refaultable(") + '\n' + extract_block(text, "static int cow_file_range_refaultable(") + '\n' + functions
        functions += '\n' + extract_block(text, "int a20_cow_share_anon_batch_snapshot(")
        release = text[text.index("void a20_cow_release_ref("):text.index("int mm_fork_clone_page(")]
        fixture = (ROOT / "tools/tests/cow_snapshot_token_host.c").read_text()
        class_start = text.index("#if defined(ARCH_HAS_PGTABLE_OPS)\nstatic uint8_t mm_fork_page_class")
        class_end = text.index("#ifdef CONFIG_SWAP\nint mm_clone_swap_page", class_start)
        header = (ROOT / "kernel/include/mm/pt.h").read_text().splitlines()
        names = ["MM_ST_INVALID", "MM_ST_PT_NODE", "MM_ST_ANON_MAPPED", "MM_ST_ANON_SHARED", "MM_ST_VMO",
                 "MM_ST_FILE_SHARED", "MM_ST_FILE_PRIVATE", "MM_ST_CLS_BYTE", "MM_ST_GET_CLASS"]
        defines = "\n".join(next(line for line in header if line.startswith("#define " + name + " ") or
                                    line.startswith("#define " + name + "(")) for name in names)
        limit = next(line for line in (ROOT / "kernel/include/mm/mm.h").read_text().splitlines() if line.startswith("#define A20_COW_ANON_BATCH_PAGES "))
        fixture = fixture.replace("#define A20_COW_ANON_BATCH_PAGES 16", limit)
        fixture = fixture.replace("/* CLASS_HELPER */", text[class_start:class_end])
        fixture = fixture.replace("/* CLASS_DEFINITIONS */", defines)
        fixture = fixture.replace("/* SNAPSHOT_STRUCT */", struct).replace("/* SNAPSHOT_FUNCTIONS */", functions).replace("/* RELEASE_FUNCTION */", release)
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "token.c", Path(tmp) / "token"
            src.write_text(fixture)
            for no_exec_fault in (0, 1):
                subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                                "-fsanitize=address,undefined", "-DARCH_HAS_PGTABLE_OPS=1",
                                f"-DARCH_INSN_FAULT_UNRECOVERABLE={no_exec_fault}",
                                "-DARCH_HAS_PTE_BLOCK=1", str(src), "-o", str(exe)], check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
