#!/usr/bin/env python3
"""Audit retained Minecraft runs without booting or rewriting their reports.

--screenshots-reviewed records a separate visual inspection of the listed
welcome/menu/options/quit images. Hashes and existence alone do not verify UI.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("shared_runner", ROOT / "tools/check-multi-qemu-kernel.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("runs", type=Path, nargs="+")
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--screenshots-reviewed", action="store_true")
args = parser.parse_args()
if args.out.exists(): parser.error("refuse to overwrite an earlier audit")
rows = []
for directory in args.runs:
    directory = directory.resolve()
    report = json.loads((directory / "report.json").read_text())
    if not report["passed"] or report["workload"] != "minecraft" or report["minecraft_exit"] != 0:
        raise RuntimeError("missing successful Minecraft lifecycle: " + str(directory))
    if report["system_image_before_sha256"] != report["system_image_after_sha256"]:
        raise RuntimeError("system backing changed")
    files = {"kernel.elf": report["kernel_sha256"], "minecraft-payload.img": report["minecraft_payload_sha256"],
             "jvm-root.qcow2": report["jvm_overlay_sha256"], **report["log_sha256"]}
    if report["mode"] == "shared": files["reset.bin"] = report["reset_sha256"]
    for name, digest in report["minecraft_files"].items(): files["minecraft-payload/" + name] = digest
    for name, expected in files.items():
        if runner.sha(directory / name) != expected: raise RuntimeError("changed artifact: " + name)
    content = (directory / "node-0.log").read_text(errors="replace")
    threads = runner.minecraft_thread_evidence(content)
    if "MINECRAFT_OWNER: appctl exit status=0" not in content or "CLUSTER_MINECRAFT: session-exit=0" not in content:
        raise RuntimeError("missing guest exit markers")
    ordinary_isolated = None
    if report["mode"] == "smp":
        symbols = subprocess.check_output(["nm", str(directory / "kernel.elf")], text=True)
        if " shared_mmio_read" in symbols or " shared_device_call" in symbols:
            raise RuntimeError("ordinary kernel contains shared owner path")
        ordinary_isolated = True
    images = ["welcome.png", "main-menu-ready.png", "options.png", "quit-selected.png"]
    complete_images = all((directory / name).is_file() for name in images)
    interaction = directory / "interaction.jsonl"
    keys = set()
    if interaction.is_file():
        for line in interaction.read_text().splitlines():
            record = json.loads(line)
            request = record.get("request", {})
            if request.get("execute") == "input-send-event" and "return" in record.get("reply", {}):
                for event in request["arguments"]["events"]:
                    if event["type"] == "key": keys.add(event["data"]["key"]["data"])
    visual = args.screenshots_reviewed and complete_images and {"tab", "ret", "esc"} <= keys
    hashes = {name: runner.sha(directory / name) for name in files}
    for name in ("report.json", "source.diff", "running.json", "interaction.jsonl", *images):
        if (directory / name).is_file(): hashes[name] = runner.sha(directory / name)
    rows.append({"run": str(directory), "mode": report["mode"], "lifecycle_passed": True,
                 "visual_and_input_reviewed": visual, "ordinary_owner_path_absent": ordinary_isolated,
                 "thread_evidence": threads, "artifact_sha256": hashes,
                 "system_backing_sha256": report["system_image_before_sha256"]})
result = {"scope": "x86_64 Minecraft client startup/menu/input/exit; not MCU or other ISA acceptance",
          "checker_sha256": runner.sha(Path(__file__)), "thread_parser_sha256": runner.sha(ROOT / "tools/check-multi-qemu-kernel.py"),
          "screenshots_reviewed": args.screenshots_reviewed, "runs": rows}
with args.out.open("x") as stream: stream.write(json.dumps(result, indent=2) + "\n")
print("PASS artifact audit:", len(rows), "runs; visual/input reviewed:", sum(row["visual_and_input_reviewed"] for row in rows))
