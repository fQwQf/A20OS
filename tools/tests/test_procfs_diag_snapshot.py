"""Execute real diagnostic growth/formatting/profiling code with fault injection."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DiagnosticSnapshot(unittest.TestCase):
    def test_complete_maximum_snapshot_and_allocation_failures(self):
        proc = (ROOT / "kernel/fs/procfs/procfs.c").read_text()
        helper = proc[proc.index("#define PROCFS_DIAG_MAX_CONTENT"):proc.index("static procfs_priv_t *procfs_priv_create(")]
        dispatch = (ROOT / "kernel/syscall/syscall.c").read_text()
        profile = dispatch[dispatch.index("static inline uint64_t syscall_profile_now("):dispatch.index("void syscall_profile_reset(")]
        fixture = (ROOT / "tools/tests/procfs_diag_snapshot_host.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "core").mkdir()
            (d / "sys").mkdir()
            (d / "core/cpu.h").write_text("#pragma once\n#include <stdint.h>\n#include <stddef.h>\nstatic inline unsigned arch_current_cpu_id(void) { return 0; }\n")
            (d / "core/stdio.h").write_text("#include <stdio.h>\n")
            (d / "core/string.h").write_text("#include <string.h>\n")
            (d / "sys/syscall.h").write_text("#pragma once\n#define SYSCALL_PROFILE_MAX 1024\ntypedef struct { uint64_t count,cycles; } syscall_prof_t;\nextern syscall_prof_t sys_prof[SYSCALL_PROFILE_MAX];\n")
            src, exe = d / "fixture.c", d / "fixture"
            src.write_text(fixture.replace("/* PRODUCTION_GROW */", helper).replace("/* PRODUCTION_PROFILE */", profile))
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I" + str(d),
                            "-I" + str(ROOT / "kernel/include"), str(src),
                            str(ROOT / "kernel/core/perf.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
