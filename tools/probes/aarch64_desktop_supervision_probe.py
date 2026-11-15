#!/usr/bin/env python3
"""Run the Native recovery gate inside a private XFCE/OpenRC desktop world.

The supplied world and FAT are copied before injecting the test service and
Native binaries. The private disks must shut down normally and pass fsck.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/cli"))
from a20_resource import Policy, Requirement, gate
from aarch64_supervision_probe import validate


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def qmp_screen(sockpath, target):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(3)
        sock.connect(str(sockpath))
        stream = sock.makefile("rwb")
        stream.readline()
        for command in ({"execute": "qmp_capabilities"},
                        {"execute": "screendump", "arguments": {"filename": str(target.resolve())}}):
            stream.write((json.dumps(command) + "\n").encode())
            stream.flush()
            while True:
                reply = json.loads(stream.readline())
                if "error" in reply:
                    raise RuntimeError(reply["error"])
                if "return" in reply:
                    break


def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"passed": False, "scope": "AArch64 SMP4 XFCE/OpenRC with Native supervised services",
              "kernel_sha256": sha(args.kernel), "world_sha256": sha(args.world),
              "fat_sha256": sha(args.fat), "native_sha256": {}}
    output = bytearray()
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="a20-desktop-supervision-") as temporary:
            work = Path(temporary)
            world, fat, scratch = (work / name for name in ("world.img", "fat.img", "scratch.img"))
            shutil.copyfile(args.world, world)
            shutil.copyfile(args.fat, fat)
            with scratch.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            subprocess.run(["/usr/sbin/mkfs.fat", "-F", "32", str(scratch)], check=True,
                           stdout=subprocess.DEVNULL)
            # debugfs applies these only to the private world. rm may report a
            # missing file on a fresh world; every subsequent write is verified.
            edits = []
            files = [(ROOT / "tools/probes/desktop_supervision_guest.sh", "/usr/lib/a20/supervision-qa.sh"),
                     (ROOT / "tools/probes/desktop_supervision.openrc", "/etc/init.d/a20-supervision-qa")]
            for name in ("svcmgr-aarch64", "rtcd-aarch64.a20drv", "svc-echod-aarch64",
                         "ufsd-aarch64", "native-registry-aarch64"):
                source = args.user_build_dir / name
                result["native_sha256"][name] = sha(source)
                files.append((source, "/bin/" + name))
            for index, (source, target) in enumerate(files):
                # Stage simple names so paths containing spaces cannot alter
                # the debugfs command language.
                staged = work / ("inject-" + str(index))
                shutil.copyfile(source, staged)
                edits += ["rm " + target, f"write {staged} {target}",
                          "set_inode_field " + target + " mode 0100755"]
            edits += ["rm /etc/runlevels/default/a20-supervision-qa",
                      "symlink /etc/runlevels/default/a20-supervision-qa ../../init.d/a20-supervision-qa"]
            batch = work / "inject.commands"
            batch.write_text("\n".join(edits) + "\n")
            injected = subprocess.run(["/usr/sbin/debugfs", "-w", "-f", str(batch), str(world)],
                                      capture_output=True, text=True)
            (args.output / "injection.log").write_text(injected.stdout + injected.stderr)
            if injected.returncode:
                raise RuntimeError("debugfs injection failed")
            for source, target in files:
                dumped = work / "verify-file"
                dumped.unlink(missing_ok=True)
                subprocess.run(["/usr/sbin/debugfs", "-R", f"dump {target} {dumped}", str(world)],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if not dumped.is_file() or sha(dumped) != sha(source):
                    raise RuntimeError("injected file mismatch: " + target)
            result["prepared_world_sha256"] = sha(world)
            qmp = work / "qmp.sock"
            command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57", "-m", "2G",
                       "-smp", "4", "-no-reboot", "-display", "none", "-serial", "stdio", "-monitor", "none",
                       "-global", "virtio-mmio.force-legacy=false"]
            for slot, image in enumerate((fat, world, scratch)):
                command += ["-drive", f"file={image},if=none,format=raw,id=d{slot}",
                            "-device", f"virtio-blk-device,drive=d{slot},bus=virtio-mmio-bus.{slot}"]
            command += ["-netdev", "user,id=net", "-device", "virtio-net-device,netdev=net,bus=virtio-mmio-bus.4",
                        "-device", "virtio-keyboard-device,bus=virtio-mmio-bus.5", "-device", "virtio-mouse-device,bus=virtio-mmio-bus.6",
                        "-device", "virtio-gpu-device,bus=virtio-mmio-bus.7", "-qmp", f"unix:{qmp},server=on,wait=off",
                        "-kernel", str(args.kernel.resolve()), "-append", "a20.ufsd_blk=2"]
            (args.output / "argv.json").write_text(json.dumps(command, indent=2) + "\n")
            with subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, bufsize=0) as guest:
                with selectors.DefaultSelector() as poller, (args.output / "guest.log").open("wb") as log:
                    poller.register(guest.stdout, selectors.EVENT_READ)
                    deadline, screenshot = time.monotonic() + args.timeout, False
                    try:
                        while guest.poll() is None and time.monotonic() < deadline:
                            for key, _ in poller.select(.05):
                                data = os.read(key.fd, 65536)
                                if not data:
                                    poller.unregister(key.fileobj)
                                else:
                                    output.extend(data)
                                    log.write(data)
                                    log.flush()
                            if not screenshot and b"A20_DESKTOP_SUPERVISION: READY" in output:
                                qmp_screen(qmp, args.output / "desktop.ppm")
                                screenshot = True
                        if guest.poll() is None:
                            raise RuntimeError("desktop gate timed out (killed)")
                        rest = guest.stdout.read() or b""
                        output.extend(rest)
                        log.write(rest)
                        result["qemu_exit"] = guest.returncode
                        if guest.returncode:
                            raise RuntimeError("QEMU did not exit normally")
                    finally:
                        if guest.poll() is None:
                            guest.kill()
                            guest.wait()
            text = output.decode(errors="replace").replace("\r", "")
            validate(text, "supervisor", 3)
            for marker in ("READY labwc and xfce4-panel", "UFSD_IO_PASS", "UFSD_UNMOUNT_PASS", "PASS cycles=3"):
                if "A20_DESKTOP_SUPERVISION: " + marker not in text:
                    raise RuntimeError("missing desktop check: " + marker)
            if "A20_DESKTOP_SUPERVISION: FAIL" in text or not screenshot:
                raise RuntimeError("desktop check failed")
            for name, image, checker in (("world", world, ["/usr/sbin/e2fsck", "-fn"]),
                                          ("scratch", scratch, ["/usr/sbin/fsck.fat", "-n"])):
                checked = subprocess.run(checker + [str(image)], capture_output=True, text=True)
                (args.output / (name + ".fsck")).write_text(checked.stdout + checked.stderr)
                result[name + "_fsck_exit"] = checked.returncode
                if checked.returncode:
                    raise RuntimeError(name + " postimage consistency check failed")
            if sha(args.world) != result["world_sha256"] or sha(args.fat) != result["fat_sha256"]:
                raise RuntimeError("input image changed")
            result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
    finally:
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (args.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2), flush=True)
    return 0 if result["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True, help="kernel.bin Image")
    parser.add_argument("--fat", type=Path, required=True)
    parser.add_argument("--world", type=Path, required=True)
    parser.add_argument("--user-build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=240)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    policy = Policy.from_env()
    gate(Requirement(2048 + policy.reserve_mem_mb, 4, policy.min_disk_mb),
         policy, ROOT, wait=False, label="desktop supervision")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
