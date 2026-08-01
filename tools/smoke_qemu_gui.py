#!/usr/bin/env python3
"""Behavioral smoke test for A20OS QEMU display and input drivers."""

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time


def wait_for(predicate, timeout, description):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise RuntimeError(f"timed out waiting for {description}")


def read_log(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as stream:
            return stream.read()
    except FileNotFoundError:
        return ""


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect(path)
        self.file = self.sock.makefile("rwb", buffering=0)
        self._read_reply("QMP greeting")
        self.execute("qmp_capabilities")

    def _read_reply(self, description):
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError(f"QEMU closed QMP while reading {description}")
            message = json.loads(line)
            if "event" in message:
                continue
            if "error" in message:
                raise RuntimeError(f"QMP {description} failed: {message['error']}")
            return message

    def execute(self, command, arguments=None):
        request = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        self.file.write((json.dumps(request) + "\n").encode())
        return self._read_reply(command).get("return")

    def close(self):
        self.file.close()
        self.sock.close()


def ppm_has_visible_content(path):
    with open(path, "rb") as stream:
        if stream.readline().strip() != b"P6":
            raise RuntimeError("QEMU screendump is not a binary PPM")
        line = stream.readline()
        while line.startswith(b"#"):
            line = stream.readline()
        width, height = map(int, line.split())
        if int(stream.readline()) != 255:
            raise RuntimeError("QEMU screendump has an unsupported color depth")
        pixels = stream.read()
    if len(pixels) != width * height * 3:
        raise RuntimeError("QEMU screendump is truncated")
    stride = max(3, (len(pixels) // 20000 // 3) * 3)
    colors = {pixels[i:i + 3] for i in range(0, len(pixels) - 2, stride)}
    return width >= 640 and height >= 480 and len(colors) >= 8


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=("x86_64", "riscv64", "aarch64", "arm32", "loongarch64"), required=True)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--artifacts", default=".kernel-build/smoke/qemu-gui-x86_64")
    args = parser.parse_args()

    for path in (args.kernel, args.disk):
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            raise RuntimeError(f"required image is missing or empty: {path}")

    os.makedirs(args.artifacts, exist_ok=True)
    log_path = os.path.join(args.artifacts, "serial.log")
    shot_path = os.path.abspath(os.path.join(args.artifacts, "screen.ppm"))
    for path in (log_path, shot_path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    with tempfile.TemporaryDirectory(prefix="a20os-gui-smoke-") as temp:
        qmp_path = os.path.join(temp, "qmp.sock")
        common = [
            args.qemu, "-m", "1G", "-smp", "1", "-display", "none",
            "-serial", f"file:{os.path.abspath(log_path)}", "-monitor", "none",
            "-qmp", f"unix:{qmp_path},server=on,wait=off", "-no-reboot",
            "-drive", f"file={os.path.abspath(args.disk)},if=none,format=raw,id=x0",
        ]
        if args.arch in ("x86_64", "loongarch64"):
            machine = [
                "-machine", "q35" if args.arch == "x86_64" else "virt",
                "-vga", "none",
                "-device", "virtio-blk-pci,drive=x0",
                "-device", "virtio-gpu-pci",
                "-device", "virtio-keyboard-pci",
                "-device", "virtio-mouse-pci",
            ]
        else:
            cpu = []
            firmware = []
            if args.arch == "riscv64":
                firmware = ["-bios", "default"]
            if args.arch == "aarch64":
                cpu = ["-cpu", "cortex-a57"]
            elif args.arch == "arm32":
                cpu = ["-cpu", "cortex-a15"]
            machine = [
                "-machine", "virt",
                "-global", "virtio-mmio.force-legacy=false",
                "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
                "-device", "virtio-keyboard-device,bus=virtio-mmio-bus.5",
                "-device", "virtio-mouse-device,bus=virtio-mmio-bus.6",
                "-device", "virtio-gpu-device,bus=virtio-mmio-bus.7",
            ] + firmware + cpu
        command = common + machine + ["-kernel", os.path.abspath(args.kernel)]
        with open(os.devnull, "wb") as devnull:
            process = subprocess.Popen(command, stdout=devnull, stderr=devnull)
        qmp = None
        try:
            wait_for(lambda: os.path.exists(qmp_path) or process.poll() is not None,
                     10, "QMP socket")
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited early with status {process.returncode}")
            qmp = Qmp(qmp_path)

            def drivers_ready():
                log = read_log(log_path)
                return ("[GPU] virtio-gpu ready:" in log and
                        log.count("[INPUT] virtio-input ready") >= 2 and
                        ("[desktop] framebuffer ready" in log or
                         "Mission Control initialized, entering loop" in log or
                         "Desktop and terminal initialized, entering loop" in log))

            wait_for(drivers_ready, args.timeout, "GPU, keyboard, mouse, and desktop")
            # "entering loop" is printed immediately before LVGL's first timer
            # pass.  Retry screendump until that pass has rendered and flushed.
            def visible_scanout():
                try:
                    qmp.execute("screendump", {"filename": shot_path})
                    return ppm_has_visible_content(shot_path)
                except (FileNotFoundError, RuntimeError):
                    return False

            wait_for(visible_scanout, 15, "non-blank framebuffer scanout")
            if "[GPU] send_cmd TIMEOUT" in read_log(log_path):
                raise RuntimeError("virtio-gpu command timed out during desktop refresh")

            before = read_log(log_path).count("[INPUT] event type=")
            qmp.execute("human-monitor-command", {"command-line": "sendkey a"})
            wait_for(lambda: read_log(log_path).count("[INPUT] event type=") > before,
                     10, "injected keyboard event")
            print(f"smoke-qemu-gui-{args.arch}: PASS (log={log_path}, screenshot={shot_path})",
                  flush=True)
        except Exception:
            log = read_log(log_path)
            if log:
                print("--- serial log tail ---", file=sys.stderr)
                print("\n".join(log.splitlines()[-100:]), file=sys.stderr)
            raise
        finally:
            if qmp is not None:
                try:
                    qmp.execute("quit")
                except Exception:
                    pass
                qmp.close()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"smoke-qemu-gui: FAIL: {error}", file=sys.stderr)
        sys.exit(1)
