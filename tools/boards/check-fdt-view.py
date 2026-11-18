#!/usr/bin/env python3
"""Compile and fuzz the production allocation-free FDT reader with sanitizers."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from raspberrypi4_fixture import board_dtb

output = ROOT / ".kernel-build/cluster/fdt-view"
output.mkdir(parents=True, exist_ok=True)
blob = output / "fixture.dtb"
blob.write_bytes(board_dtb())
binary = output / "fdt-view-test"
command = ["gcc", "-std=gnu99", "-O1", "-g", "-fno-omit-frame-pointer",
           "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
           "-I" + str(ROOT / "kernel/include"),
           str(ROOT / "kernel/core/fdt_view.c"),
           str(ROOT / "tests/tools/tests/fdt_view.c"),
           "-o", str(binary)]
subprocess.run(command, cwd=ROOT, check=True)
result = subprocess.run([str(binary), str(blob)], cwd=ROOT, check=True, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
(output / "test.log").write_text(result.stdout)
(output / "report.json").write_text(json.dumps({"passed": True, "command": command,
    "fixture_sha256": hashlib.sha256(blob.read_bytes()).hexdigest(),
    "output": result.stdout, "sanitizers": ["ASan", "UBSan"]}, indent=2) + "\n")
print(result.stdout, end="")
