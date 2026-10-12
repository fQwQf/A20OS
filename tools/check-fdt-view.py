#!/usr/bin/env python3
"""Compile and fuzz the production allocation-free FDT reader with sanitizers."""
import hashlib
import json
from pathlib import Path
import subprocess
from raspberrypi4_fixture import board_dtb

output = Path(".kernel-build/cluster/fdt-view")
output.mkdir(parents=True, exist_ok=True)
blob = output / "fixture.dtb"
blob.write_bytes(board_dtb())
binary = output / "fdt-view-test"
command = ["gcc", "-std=gnu99", "-O1", "-g", "-fno-omit-frame-pointer",
           "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
           "-Ikernel/include", "kernel/core/fdt_view.c", "tools/tests/fdt_view.c",
           "-o", str(binary)]
subprocess.run(command, check=True)
result = subprocess.run([str(binary), str(blob)], check=True, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
(output / "test.log").write_text(result.stdout)
(output / "report.json").write_text(json.dumps({"passed": True, "command": command,
    "fixture_sha256": hashlib.sha256(blob.read_bytes()).hexdigest(),
    "output": result.stdout, "sanitizers": ["ASan", "UBSan"]}, indent=2) + "\n")
print(result.stdout, end="")
