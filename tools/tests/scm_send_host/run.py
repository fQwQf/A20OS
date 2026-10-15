#!/usr/bin/env python3
"""Compile the exact SCM send-acquisition production helper against a host fd seam."""
from __future__ import annotations
import datetime
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
source = ROOT / "kernel/abi/linux/sys_socket_msg.c"
template = ROOT / "tools/tests/scm_send_host/harness.c.in"

def extract_function(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{": depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise ValueError("unterminated production helper")

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
base = Path(os.environ.get("SCM_SEND_EVIDENCE", "<build-dir>/secondary-linux-abi-batch4/scm-send-host")) / f"scm-send-host-{stamp}-{os.getpid()}"
base.mkdir(parents=True, exist_ok=False)
text = source.read_text()
function = extract_function(text, "static int linux_scm_acquire_send_fd(")
code = template.read_text().replace("/* PRODUCTION_CALLSITE */", function)
unit = base / "harness.c"
unit.write_text(code)
binary = base / "harness"
cc = os.environ.get("CC", "cc")
argv = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", str(unit), "-o", str(binary)]
version = subprocess.run([cc, "--version"], text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
(base / "toolchain.log").write_text(version.stdout)
(base / "toolchain.exit").write_text(f"{version.returncode}\n")
(base / "inputs.sha256").write_text(
    f"{sha(source)}  kernel/abi/linux/sys_socket_msg.c\n"
    f"{sha(template)}  tools/tests/scm_send_host/harness.c.in\n"
    f"{sha(Path(__file__))}  tools/tests/scm_send_host/run.py\n")
(base / "compile.argv").write_text(shlex.join(argv) + "\n")
cp = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(base / "compile.log").write_text(cp.stdout)
(base / "compile.exit").write_text(f"{cp.returncode}\n")
if cp.returncode:
    print(f"compile failed; evidence: {base}", file=sys.stderr)
    sys.exit(cp.returncode)
run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(base / "run.argv").write_text(shlex.join([str(binary)]) + "\n")
(base / "run.log").write_text(run.stdout)
(base / "run.exit").write_text(f"{run.returncode}\n")
(base / "binary.sha256").write_text(f"{sha(binary)}  harness\n")
print(run.stdout, end="")
print(f"SCM_SEND_HOST_EVIDENCE={base}")
sys.exit(run.returncode)
