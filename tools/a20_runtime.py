"""Named headless QEMU hosts. Runtime state is separate from build templates.

Only start() builds or launches. list/attach/control never invoke the build
engine. Each run has private writable disk overlays and a local QMP endpoint.
"""
from __future__ import annotations

import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import socket
import subprocess
import sys
import termios
import time
import tty

from a20_error import A20Error
from a20_make import REPO_ROOT, build_instance
from a20_test import _qemu_cmdline

_NAME = re.compile(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,31}\Z")


def _root() -> Path:
    parent = Path(os.environ.get("XDG_RUNTIME_DIR", f"/tmp/a20-{os.getuid()}"))
    parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    pst = parent.lstat()
    if parent.is_symlink() or pst.st_uid != os.getuid() or pst.st_mode & 0o022:
        raise A20Error(f"runtime parent is not private to this user: {parent}")
    root = parent / "a20-runs"
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    st = root.lstat()
    if root.is_symlink() or st.st_uid != os.getuid() or st.st_mode & 0o077:
        raise A20Error(f"runtime directory must be private and owned by you: {root}")
    return root


def _path(name: str) -> Path:
    if not _NAME.fullmatch(name):
        raise A20Error("runtime name must use 1–32 letters, digits, '_' or '-'")
    path = _root() / name
    if "," in str(path):
        raise A20Error("runtime paths cannot contain commas")
    if len(os.fsencode(path / "console.sock")) > 103:
        raise A20Error("XDG_RUNTIME_DIR is too long for a Unix console socket")
    if path.is_symlink():
        raise A20Error(f"runtime path is a symlink: {path}")
    return path


@contextlib.contextmanager
def _lock():
    # Serialize start/destroy, including the interval before QEMU publishes its
    # pidfile. Never unlink the lock inode while another caller may hold it.
    with (_root() / ".lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def _read(path: Path) -> dict:
    try:
        return json.loads((path / "state.json").read_text())
    except (OSError, ValueError) as exc:
        raise A20Error(f"cannot read runtime {path.name}: {exc}") from None


def _birth(pid: int) -> str | None:
    try:
        # comm can contain spaces and parentheses; fields after the final ')'
        # start with state (field 3), and starttime is field 22.
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError):
        return None


def _alive(state: dict) -> bool:
    return bool(state.get("birth") and
                _birth(state.get("pid", -1)) == state["birth"])


def _qmp(path: Path, command: str) -> dict:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(3)
        sock.connect(str(path / "qmp.sock"))
        with sock.makefile("rwb") as stream:
            def read():
                raw = stream.readline(1024 * 1024)
                if not raw:
                    raise OSError("QMP closed before acknowledging command")
                return json.loads(raw)
            if "QMP" not in read():
                raise OSError("invalid QMP greeting")
            for ident, cmd in enumerate(("qmp_capabilities", command)):
                stream.write((json.dumps({"execute": cmd, "id": ident}) + "\n").encode())
                stream.flush()
                deadline = time.monotonic() + 5
                while True:
                    reply = read()
                    if reply.get("id") == ident:
                        if "error" in reply:
                            raise OSError(str(reply["error"]))
                        break
                    if time.monotonic() >= deadline:
                        raise OSError("QMP response timed out")
            return reply.get("return", {})


def _checked(argv: list[str]) -> str:
    result = subprocess.run(argv, cwd=REPO_ROOT, capture_output=True, text=True)
    if result.returncode:
        raise A20Error(f"{argv[0]} failed: {result.stderr.strip()}")
    return result.stdout


def _prepare(argv: list[str], path: Path, name: str, *, vm_manager: bool = False) -> list[str]:
    result: list[str] = []
    i = 0
    disk = 0
    mac = "02:" + ":".join(f"{n:02x}" for n in hashlib.sha256(name.encode()).digest()[:5])
    while i < len(argv):
        flag = argv[i]
        i += 1
        if flag == "-nographic":
            continue
        if flag in {"-monitor", "-serial", "-display"}:
            if i >= len(argv):
                raise A20Error(f"missing QEMU argument for {flag}")
            value = argv[i]
            i += 1
            if (flag == "-serial" and value not in {"stdio", "mon:stdio", "none"}):
                raise A20Error("named runs require the primary serial console")
            continue
        if flag in {"-daemonize", "-qmp", "-pidfile", "-chardev", "-snapshot",
                    "-vnc", "-incoming", "-blockdev", "-hda", "-hdb", "-sd"}:
            raise A20Error(f"named runs cannot safely rewrite QEMU option {flag}")
        if flag == "-drive":
            if i >= len(argv):
                raise A20Error("missing -drive value")
            value = argv[i]
            i += 1
            if ",," in value:
                raise A20Error("escaped commas in disk paths are unsupported for named runs")
            parts = value.split(",")
            opts = dict(item.split("=", 1) for item in parts if "=" in item)
            if "file" not in opts or "snapshot" in opts:
                raise A20Error("named runs need explicit file-backed drives without snapshot options")
            source = Path(opts["file"])
            if not source.is_absolute():
                source = REPO_ROOT / source
            source = source.resolve(strict=True)
            if "," in str(source):
                raise A20Error("commas in disk paths are unsupported for named runs")
            if opts.get("readonly") not in {"on", "yes"}:
                # Build outputs may be rebuilt while this run is alive. A
                # private flattened base also breaks external backing chains.
                base = path / f"base-{disk}.qcow2"
                _checked(["qemu-img", "convert", "-O", "qcow2", str(source), str(base)])
                base.chmod(0o400)
                overlay = path / f"disk-{disk}.qcow2"
                disk += 1
                _checked(["qemu-img", "create", "-f", "qcow2", "-F", "qcow2",
                          "-b", str(base), str(overlay)])
                parts = [p for p in parts if not p.startswith(("file=", "format="))]
                parts += [f"file={overlay}", "format=qcow2"]
            else:
                parts = [f"file={source}" if p.startswith("file=") else p for p in parts]
            result += [flag, ",".join(parts)]
        elif flag == "-device" and i < len(argv):
            value = argv[i]
            i += 1
            if "netdev=" in value and ",mac=" not in value:
                value += f",mac={mac}"
            result += [flag, value]
        else:
            result.append(flag)
    if vm_manager:
        # Preserve existing network/boot arguments, but make one -append value.
        bootargs = []
        filtered = []
        j = 0
        while j < len(result):
            if result[j] == "-append":
                if j + 1 >= len(result):
                    raise A20Error("missing -append value")
                bootargs.append(result[j + 1]); j += 2
            else:
                filtered.append(result[j]); j += 1
        result = filtered + ["-append", " ".join(bootargs + ["a20.vmd=1"])]
        result += ["-chardev", f"socket,id=a20management,path={path / 'control.sock'},server=on,wait=off",
                   "-device", "virtio-serial-pci,id=a20managementbus",
                   "-device", "virtconsole,chardev=a20management,bus=a20managementbus.0"]
    result += ["-display", "none", "-monitor", "none", "-daemonize",
               "-pidfile", str(path / "qemu.pid"),
               "-qmp", f"unix:{path / 'qmp.sock'},server=on,wait=off",
               "-chardev", f"socket,id=a20console,path={path / 'console.sock'},server=on,wait=off,logfile={path / 'console.log'}",
               "-serial", "chardev:a20console"]
    return result


def start(inst, name: str, *, detach: bool, no_build: bool, dry_run: bool,
          make_args: list[str]) -> None:
    if inst.gui.enabled or inst.rootfs.world is not None:
        raise A20Error("named runs currently support headless base-image instances")
    if make_args:
        raise A20Error("named runs require build settings in the instance manifest")
    if not _NAME.fullmatch(name):
        raise A20Error("invalid runtime name")
    if dry_run:
        if not no_build:
            build_instance(inst, [], True)
        print(f"would start {inst.name} as {name} with private disk overlays, QMP and console")
        return
    with _lock():
        path = _path(name)
        if path.exists():
            raise A20Error(f"runtime {name} already exists; use attach or destroy it first")
        if not no_build:
            build_instance(inst, [], False)
        argv = _qemu_cmdline(inst)
        path.mkdir(mode=0o700)
        state = {"schema": 1, "name": name, "template": inst.name,
                 "repository": str(REPO_ROOT), "pid": None, "birth": None,
                 "vm_manager": bool(inst.machine.vm_manager)}
        (path / "state.json").write_text(json.dumps(state, indent=2) + "\n")
        try:
            argv = _prepare(argv, path, name, vm_manager=bool(inst.machine.vm_manager))
            (path / "argv.json").write_text(json.dumps(argv, indent=2) + "\n")
            _checked(argv)
            pid = int((path / "qemu.pid").read_text())
            state.update(pid=pid, birth=_birth(pid))
            (path / "state.json").write_text(json.dumps(state, indent=2) + "\n")
            _qmp(path, "query-status")
        except Exception:
            # Keep partial state and diagnostics; never delete disks under a
            # QEMU whose startup result is uncertain. destroy checks identity.
            if (path / "qemu.pid").exists():
                pid = int((path / "qemu.pid").read_text())
                state.update(pid=pid, birth=_birth(pid))
                (path / "state.json").write_text(json.dumps(state, indent=2) + "\n")
            raise
    print(f"started {name}; console log: {path / 'console.log'}")
    if not detach:
        attach(name)


def attach(name: str) -> None:
    path = _path(name)
    if not _alive(_read(path)):
        raise A20Error(f"runtime {name} is not running")
    # A separate lock prevents a second console writer from stealing input.
    with (path / "console.lock").open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise A20Error(f"runtime {name} already has an attached console") from None
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.connect(str(path / "console.sock"))
            fd = sys.stdin.fileno()
            old = termios.tcgetattr(fd) if os.isatty(fd) else None
            # Show recent boot output before switching the terminal to raw mode.
            with contextlib.suppress(OSError):
                with (path / "console.log").open("rb") as log:
                    log.seek(max(0, log.seek(0, 2) - 16384))
                    sys.stdout.buffer.write(log.read())
            print("Ctrl-] detaches; the instance keeps running.", flush=True)
            try:
                if old is not None:
                    tty.setraw(fd)
                while True:
                    ready, _, _ = select.select([fd, sock], [], [])
                    if sock in ready:
                        data = sock.recv(65536)
                        if not data:
                            return
                        sys.stdout.buffer.write(data)
                        sys.stdout.buffer.flush()
                    if fd in ready:
                        data = os.read(fd, 4096)
                        if not data:
                            return
                        if b"\x1d" in data:
                            before = data.split(b"\x1d", 1)[0]
                            if before:
                                sock.sendall(before)
                            return
                        sock.sendall(data)
            finally:
                if old is not None:
                    termios.tcsetattr(fd, termios.TCSADRAIN, old)


def control(name: str, action: str) -> None:
    with _lock():
        path = _path(name)
        state = _read(path)
        live = _alive(state)
        if action == "destroy":
            if live:
                raise A20Error(f"stop {name} before destroying its writable disks")
            shutil.rmtree(path)
            return
        if not live:
            raise A20Error(f"runtime {name} is not running")
        command = {"pause": "stop", "resume": "cont", "stop": "quit"}[action]
        try:
            _qmp(path, command)
        except (OSError, ValueError) as exc:
            if action != "stop" or _alive(state):
                raise A20Error(f"QMP {action} failed: {exc}") from None
        if action == "stop":
            deadline = time.monotonic() + 5
            while _alive(state) and time.monotonic() < deadline:
                time.sleep(0.05)
            if _alive(state):
                raise A20Error(f"{name} acknowledged quit but has not exited")


def list_runs() -> None:
    for path in sorted(_root().iterdir()):
        if not path.is_dir() or path.is_symlink():
            continue
        try:
            state = _read(path)
            status = "stopped"
            if _alive(state):
                try:
                    status = _qmp(path, "query-status").get("status", "unknown")
                except (OSError, ValueError):
                    status = "running (QMP unavailable)"
            print(f"{path.name}\t{state['template']}\t{status}")
        except A20Error as exc:
            print(f"{path.name}\t{exc}", file=sys.stderr)
