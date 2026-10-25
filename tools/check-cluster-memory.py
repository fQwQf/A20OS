#!/usr/bin/env python3
"""Production Rust state/concurrency and C ABI/config/SDK tests; no kernel boot."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    rust = shlex.split(os.environ.get("RUSTC", "rustc"))
    cc = shlex.split(os.environ.get("HOST_CC", "cc"))
    source = "user/svc/cluster_memory_state.rs"
    with tempfile.TemporaryDirectory(prefix="a20-memory-") as tmp:
        path = Path(tmp)
        def run(command, **kwargs):
            subprocess.run(command, cwd=ROOT, check=True, **kwargs)
        for pages in (1, 8, 64):
            env = dict(os.environ, A20_MEMORY_MAX_PAGES=str(pages))
            tests = path / f"state-{pages}"
            run(rust + ["--edition=2021", "--test", source, "-o", str(tests)], env=env)
            run([str(tests)])
        library = path / "memory.a"
        run(rust + ["--edition=2021", "--crate-type", "staticlib", "-C", "panic=abort",
                    "-C", "opt-level=2", source, "-o", str(library)],
            env=dict(os.environ, A20_MEMORY_MAX_PAGES="8"))
        binary = path / "c-abi"
        run(cc + ["-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                  "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                  "-Wl,--gc-sections", "tools/tests/test_clx_memory.c", str(library),
                  "-lpthread", "-ldl", "-lm", "-o", str(binary)])
        run([str(binary)])
    print("check-cluster-memory: PASS")


if __name__ == "__main__":
    main()
