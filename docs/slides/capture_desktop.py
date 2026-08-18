#!/usr/bin/env python3
"""截取 A20OS riscv64 GUI 桌面实拍图（答辩 slides 用）。

前置：构建 .kernel-build/riscv64-qemu-virt-riscv64-both-dev/{kernel.elf, gui-fat32.img}
     （见 make smoke-qemu-gui-riscv64 的构建步骤）。
用法：python3 docs/slides/capture_desktop.py <输出.ppm>
输出为 PPM，可用 PIL/ImageMagick 转 PNG 后放入 docs/slides/figures/。
"""
import json, os, socket, subprocess, sys, tempfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
KERNEL = f"{ROOT}/.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf"
DISK = f"{ROOT}/.kernel-build/riscv64-qemu-virt-riscv64-both-dev/gui-fat32.img"
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/opencode/screen2.ppm"


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        self.sock.connect(path)
        self.f = self.sock.makefile("rwb", buffering=0)
        self.cmd({"execute": "qmp_capabilities"})

    def cmd(self, request):
        self.f.write((json.dumps(request) + "\n").encode())
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            if "error" in msg:
                raise RuntimeError(f"QMP error: {msg}")
            return msg

    def events(self, events):
        self.cmd({"execute": "input-send-event", "arguments": {"events": events}})


def abs_ev(x, y):
    return [
        {"type": "rel", "data": {"axis": "x", "value": -2000}},
        {"type": "rel", "data": {"axis": "y", "value": -2000}},
        {"type": "rel", "data": {"axis": "x", "value": 20}},
        {"type": "rel", "data": {"axis": "y", "value": 15}},
    ]


def click(q, x, y):
    q.events(abs_ev(x, y))
    time.sleep(0.3)
    q.events([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.15)
    q.events([{"type": "btn", "data": {"button": "left", "down": False}}])


def main():
    log_path = "/tmp/opencode/capture2-serial.log"
    with tempfile.TemporaryDirectory() as temp:
        qmp_path = os.path.join(temp, "qmp.sock")
        cmd = [
            "qemu-system-riscv64", "-m", "1G", "-smp", "1",
            "-display", "none", "-serial", f"file:{log_path}",
            "-monitor", "none", "-qmp", f"unix:{qmp_path},server=on,wait=off",
            "-no-reboot",
            "-drive", f"file={DISK},if=none,format=raw,id=x0",
            "-machine", "virt",
            "-global", "virtio-mmio.force-legacy=false",
            "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
            "-device", "virtio-keyboard-device,bus=virtio-mmio-bus.5",
            "-device", "virtio-mouse-device,bus=virtio-mmio-bus.6",
            "-device", "virtio-gpu-device,bus=virtio-mmio-bus.7",
            "-bios", "default",
            "-kernel", KERNEL,
        ]
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 20
            while not os.path.exists(qmp_path):
                if time.monotonic() > deadline:
                    raise RuntimeError("no QMP socket")
                time.sleep(0.1)
            q = Qmp(qmp_path)
            t0 = time.monotonic()
            while time.monotonic() - t0 < 120:
                try:
                    log = open(log_path, encoding="utf-8", errors="replace").read()
                except FileNotFoundError:
                    log = ""
                if "weston-desktop-shell" in log:
                    break
                time.sleep(1.0)
            time.sleep(15)
            # click terminal launcher icon (top-left of panel)
            click(q, int(0.018 * 32767), int(0.021 * 32767))
            time.sleep(15)
            # type commands in the terminal
            seq1 = list("uname") + ["spc", "minus", "a", "ret"]
            seq2 = list("cat") + ["spc", "slash"] + list("proc") + ["slash"] + list("a20") + ["slash"] + list("objects") + ["ret"]
            for key in seq1 + seq2:
                q.events([
                    {"type": "key", "data": {"key": {"type": "qcode", "data": key}, "down": True}},
                    {"type": "key", "data": {"key": {"type": "qcode", "data": key}, "down": False}},
                ])
                time.sleep(0.15)
            time.sleep(12)
            q.cmd({"execute": "screendump", "arguments": {"filename": OUT}})
            deadline = time.monotonic() + 30
            while not (os.path.exists(OUT) and os.path.getsize(OUT) > 0):
                if time.monotonic() > deadline:
                    raise RuntimeError("screendump did not materialize")
                time.sleep(0.5)
            print("saved", OUT)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    main()
