#!/usr/bin/env python3
"""Host checks for the production SSI lease/page state and launcher config."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="a20-ssi-") as tmp:
        out = Path(tmp)
        def run(cmd, **kw):
            return subprocess.run(cmd, cwd=ROOT, check=True, **kw)
        tests = out / "state"
        run([os.environ.get("RUSTC", "rustc"), "--edition=2021", "--test", "kernel/cluster/ssi_state.rs", "-o", str(tests)])
        run([str(tests)])
        launcher = out / "a20ssi"
        run([os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "user/cmds/core/a20ssi.c", "-o", str(launcher)])
        run([str(launcher), "docs/cluster/configs/ssi-off.conf", "/bin/true"])
        base = "version 1\nrole off\n"
        invalid = [base + "mode read-write\n", base + "version 1\n", base + "max-pages 0\n",
                   base + "max-pages 65\n", base + "max-threads 17\n", base + "max-slices 0\n",
                   base + "timeout-ms 99\n", base + "quantum-ticks 0\n", base + "port 65536\n",
                   base + "shared-memory -1\n", base + "max-bytes 42949672960\n", base + "extra value x\n",
                   base + "x" * 4097, "role off\n", "version 2\nrole off\n"]
        for text in invalid:
            config = out / "invalid.conf"
            config.write_text(text)
            r = subprocess.run([str(launcher), str(config), "/bin/true"], cwd=ROOT, capture_output=True)
            assert r.returncode == 2, (text, r.returncode, r.stderr)
        local = out / "local.conf"
        local.write_text((ROOT / "docs/cluster/configs/ssi-home.conf").read_text().replace("shared-memory 1", "shared-memory 0"))
        run([str(launcher), str(local), "/bin/true"])
        for invalid_flags in (["CLUSTER_SSI=2"], ["CLUSTER_SSI=0 1"],
                              ["CLUSTER_SSI=1", "NR_CPUS=2"], ["CLUSTER_SSI=1", "NOMMU=1"],
                              ["CLUSTER_SSI=1", "ABI=native"]):
            r = subprocess.run(["make", "-s", "ARCH=x86_64", "ABI=both", *invalid_flags, "print-build-dir"],
                               cwd=ROOT, capture_output=True)
            assert r.returncode != 0, invalid_flags
    print("check-cluster-ssi: PASS")


if __name__ == "__main__":
    main()
