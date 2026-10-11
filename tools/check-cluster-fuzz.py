#!/usr/bin/env python3
"""Bounded deterministic sanitizer run against real CL and MCU leaf code."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile
import time


def main():
    root = Path(__file__).resolve().parent.parent
    directory = root / ".kernel-build/cluster/fuzz"
    directory.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
    flags = ["-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-Ikernel/arch/riscv64/include", "-Ikernel/include", "-Ikernel"]
    inputs = ["tools/tests/test_clx_fuzz.c", "kernel/cluster/frame.c",
              "kernel/cluster/frame.h", "kernel/cluster/uart.h",
              "kernel/cluster/fragment.h", "kernel/cluster/fragment.c",
              "tools/tests/test_clx_fragment.c", "tools/tests/test_clx_session.c",
              "kernel/cluster/reliable.c", "tools/tests/test_clx_reliable.c", "kernel/cluster/session.c", "kernel/cluster/session_control.h", "kernel/mcu/leaf.c",
              "kernel/mcu/leaf.h", "kernel/include/uapi/a20/cluster_work.h",
              "kernel/include/uapi/a20/cluster_directory.h", "kernel/include/uapi/a20/cluster_config.h", "tools/tests/test_clx_config.c", "kernel/include/uapi/a20/cluster_job.h", "kernel/include/uapi/a20/cluster_words.h", "kernel/include/uapi/a20/cluster_word_job.h", "tools/tests/test_clx_word_job.c", "kernel/cluster/transaction.h"]
    hashes = {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in inputs}
    with tempfile.TemporaryDirectory(prefix="a20-cluster-fuzz-") as temporary:
        binary = str(Path(temporary) / "fuzz")
        subprocess.run(compiler + flags + [inputs[0], "-o", binary], cwd=root, check=True)
        started = time.monotonic()
        result = subprocess.run([binary], cwd=root, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        fragment = str(Path(temporary) / "fragment")
        subprocess.run(compiler + flags + ["tools/tests/test_clx_fragment.c", "-o", fragment],
                       cwd=root, check=True)
        checked = subprocess.run([fragment], cwd=root, text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=60)
        (directory / "fragment.log").write_text(checked.stdout)
        print(checked.stdout, end="")
        checked.check_returncode()
        subprocess.run(compiler + flags + ["-DCONFIG_CLUSTER_PROFILE=3",
                       "tools/tests/test_clx_fragment.c", "-o", fragment], cwd=root, check=True)
        server = subprocess.run([fragment], cwd=root, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        (directory / "fragment-server.log").write_text(server.stdout)
        print(server.stdout, end="")
        server.check_returncode()
        session = str(Path(temporary) / "session")
        subprocess.run(compiler + flags + ["tools/tests/test_clx_session.c", "-o", session], cwd=root, check=True)
        checked_session = subprocess.run([session], cwd=root, text=True, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, timeout=60)
        (directory / "session.log").write_text(checked_session.stdout)
        print(checked_session.stdout, end="")
        checked_session.check_returncode()
        config = str(Path(temporary) / "config")
        subprocess.run(compiler + flags + ["tools/tests/test_clx_config.c", "-o", config], cwd=root, check=True)
        checked_config = subprocess.run([config], cwd=root, text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=60)
        (directory / "config.log").write_text(checked_config.stdout)
        print(checked_config.stdout, end="")
        checked_config.check_returncode()
        words = str(Path(temporary) / "words")
        subprocess.run(compiler + flags + ["tools/tests/test_clx_word_job.c", "-o", words], cwd=root, check=True)
        checked_words = subprocess.run([words], cwd=root, text=True, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, timeout=60)
        (directory / "words.log").write_text(checked_words.stdout)
        print(checked_words.stdout, end="")
        checked_words.check_returncode()
        reliable = str(Path(temporary) / "reliable")
        subprocess.run(compiler + flags + ["tools/tests/test_clx_reliable.c", "-o", reliable], cwd=root, check=True)
        checked_reliable = subprocess.run([reliable], cwd=root, text=True, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, timeout=60)
        (directory / "reliable.log").write_text(checked_reliable.stdout)
        print(checked_reliable.stdout, end="")
        checked_reliable.check_returncode()
        elapsed = time.monotonic() - started
        (directory / "run.log").write_text(result.stdout)
        print(result.stdout, end="")
        result.check_returncode()
    metadata = {"compiler": subprocess.check_output(compiler + ["--version"], text=True).splitlines()[0],
                "flags": flags, "platform": platform.platform(), "seed": "0x20c1a55",
                "mutations": 1000000, "fragment_mutations": 100000, "fragment_sizes": 64118, "session_mutations": 100000, "config_mutations": 100000, "word_mutations": 100000, "elapsed_seconds": round(elapsed, 6),
                "source_sha256": hashes, "scope": "host sanitizer harness; not target throughput"}
    (directory / "evidence.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("check-cluster-fuzz: PASS (ASan/UBSan, 1M deterministic mutations)")


if __name__ == "__main__":
    main()
