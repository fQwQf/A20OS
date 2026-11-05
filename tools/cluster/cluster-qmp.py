#!/usr/bin/env python3
"""Interact with the primary of a live check-multi-qemu-kernel run.

Retains every command and response beside the run, including input events.
Screenshots and guest logs still require inspection before visual acceptance.
"""
import argparse
import json
from pathlib import Path
import socket
import struct
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("out", type=Path)
parser.add_argument("action", choices=("shot", "key", "status"))
parser.add_argument("value", nargs="?")
args = parser.parse_args()
out = args.out.resolve()
report = json.loads((out / "running.json").read_text())
command = report["commands"][0]
address = command[command.index("-qmp") + 1].split(",")[0].removeprefix("unix:")
if args.action in ("key", "shot") and not args.value:
    parser.error("key and shot require a value")
if args.action == "shot" and Path(args.value).name != args.value:
    parser.error("shot name must be a filename inside the run directory")
with socket.socket(socket.AF_UNIX) as sock:
    sock.settimeout(20)
    sock.connect(address)
    with sock.makefile("rwb", buffering=0) as stream:
        if "QMP" not in json.loads(stream.readline()):
            raise RuntimeError("invalid QMP greeting")
        def call(name, arguments=None):
            request = {"execute": name, "arguments": arguments or {}, "id": name}
            stream.write((json.dumps(request) + "\n").encode())
            while True:
                raw = stream.readline()
                if not raw: raise RuntimeError("QMP closed")
                reply = json.loads(raw)
                if reply.get("id") != name: continue
                with (out / "interaction.jsonl").open("a") as log:
                    log.write(json.dumps({"time": time.time(), "request": request, "reply": reply}) + "\n")
                if "error" in reply: raise RuntimeError(str(reply["error"]))
                return reply["return"]
        call("qmp_capabilities")
        if args.action == "key":
            keys = args.value.split("+")
            for down, group in ((True, keys), (False, list(reversed(keys)))):
                events = [{"type": "key", "data": {"down": down,
                           "key": {"type": "qcode", "data": key}}} for key in group]
                print(call("input-send-event", {"events": events}))
                if down: time.sleep(.15)
        elif args.action == "shot":
            target = out / args.value
            if target.exists(): raise RuntimeError("refuse to overwrite an earlier screenshot")
            try:
                print(call("screendump", {"filename": str(target), "format": "png"}))
            except RuntimeError as exc:
                if "no surface" not in str(exc) or "-vnc" not in command: raise
                # GL scanouts may expose a DMA-BUF rather than a QMP surface.
                # Read the actual VNC framebuffer, using lossless raw rectangles.
                from PIL import Image
                endpoint = command[command.index("-vnc") + 1].removeprefix("unix:")
                with socket.socket(socket.AF_UNIX) as vnc:
                    vnc.settimeout(20)
                    vnc.connect(endpoint)
                    def read(n):
                        data = b""
                        while len(data) < n:
                            block = vnc.recv(n - len(data))
                            if not block: raise RuntimeError("VNC closed")
                            data += block
                        return data
                    read(12); vnc.sendall(b"RFB 003.008\n")
                    types = read(read(1)[0])
                    if 1 not in types: raise RuntimeError("local VNC requires unexpected authentication")
                    vnc.sendall(b"\1")
                    if read(4) != b"\0" * 4: raise RuntimeError("VNC authentication failed")
                    vnc.sendall(b"\1")
                    width, height = struct.unpack("!HH", read(4))
                    read(16); read(struct.unpack("!I", read(4))[0])
                    vnc.sendall(b"\0\0\0\0" + struct.pack("!BBBBHHHBBBxxx", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
                    vnc.sendall(struct.pack("!BBHi", 2, 0, 1, 0))
                    vnc.sendall(struct.pack("!BBHHHH", 3, 0, 0, 0, width, height))
                    image = Image.new("RGB", (width, height))
                    while True:
                        kind = read(1)[0]
                        if kind == 0:
                            _, count = struct.unpack("!BH", read(3))
                            for _ in range(count):
                                x, y, w, h, encoding = struct.unpack("!HHHHi", read(12))
                                if encoding != 0: raise RuntimeError("VNC did not use raw rectangles")
                                image.paste(Image.frombytes("RGB", (w, h), read(w * h * 4), "raw", "BGRX"), (x, y))
                            if count: break
                        elif kind == 2: continue
                        elif kind == 3:
                            read(3); read(struct.unpack("!I", read(4))[0])
                        else: raise RuntimeError("unexpected VNC message: " + str(kind))
                    image.save(target)
                with (out / "interaction.jsonl").open("a") as log:
                    log.write(json.dumps({"time": time.time(), "vnc_framebuffer": str(target), "width": width, "height": height}) + "\n")
            print(target)
        else:
            print(call("query-status"))
