#!/usr/bin/env python3
"""Boot the same Native application synchronization test on server ISAs.

This executes real A20OS kernels and SDK syscalls in QEMU. It does not claim
physical-board coverage. MCU SVC/fabric coverage is check-mcu-program[-rtl].
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def artifact_path(path):
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def source_inputs():
    paths = ["Makefile", "tools/mk/rust-kernel.mk", "kernel/proc/task_object.c",
             "kernel/proc/task_object.rs", "user/tests/native-thread-contract.h",
             "user/tests/test_native_futex.c"]
    return {name: sha(ROOT / name) for name in paths if (ROOT / name).is_file()}


def source_tree_state():
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                   text=True).strip()
    patch = subprocess.check_output(["git", "diff", "--binary", "--no-ext-diff"],
                                    cwd=ROOT)
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=ROOT
    ).split(b"\0")
    untracked_hashes = {}
    for raw in untracked:
        if not raw:
            continue
        name = raw.decode()
        rel = Path(name)
        suffixes = {".c", ".h", ".S", ".s", ".rs", ".inc"}
        is_kernel_source = rel.parts[:1] == ("kernel",) and rel.suffix in suffixes
        is_fixture_source = name in {
            "user/tests/native-thread-contract.h", "user/tests/test_native_futex.c"}
        if not (is_kernel_source or is_fixture_source):
            continue
        path = ROOT / name
        if path.is_file():
            untracked_hashes[name] = sha(path)
    return {"head": head, "tracked_patch_sha256": hashlib.sha256(patch).hexdigest(),
            "status_porcelain": subprocess.check_output(
                ["git", "status", "--porcelain", "--untracked-files=all"],
                cwd=ROOT, text=True).splitlines(),
            "untracked_sha256": untracked_hashes}


def make_build_flags(arch, args):
    flags = [f"ARCH={arch}", "ABI=both", "BRINGUP=0", f"NR_CPUS={args.cpus}",
             f"TASK_OBJECTS_IMPL={args.task_objects_impl}"]
    if args.build_dir:
        flags.append(f"BUILD_DIR={args.build_dir}")
    if args.user_build_dir:
        flags.append(f"USER_BUILD_DIR={args.user_build_dir}")
    if args.cpus > 1:
        flags.append("ALLOW_UNVERIFIED_SMP=1")
    return flags


def native_app_path(arch, args):
    native_tag = {"riscv64": "rv", "aarch64": "aarch64",
                  "x86_64": "x86_64"}[arch]
    user_build_dir = (args.user_build_dir if args.user_build_dir
                      else ROOT / "user/build" / arch)
    return user_build_dir / f"native-futex-{native_tag}"


def prepare_snapshot(arch, args, command, kernel, image, app, build_log, source_state):
    """Copy one completed build into a content-checked, immutable run input set."""
    target = (ROOT / args.prepare_to).resolve()
    if target.exists():
        raise RuntimeError(f"refusing to overwrite prepared snapshot: {target}")
    target.mkdir(parents=True)
    shutil.copy2(kernel, target / "kernel.elf")
    shutil.copy2(image, target / "base.img")
    shutil.copy2(app, target / "native-futex")
    shutil.copy2(build_log, target / "build.log")
    (target / "proc").mkdir()
    bridge_object = kernel.parent / "proc/task_object.o"
    shutil.copy2(bridge_object, target / "proc/task_object.o")
    tracked_patch = subprocess.check_output(["git", "diff", "--binary", "--no-ext-diff"],
                                            cwd=ROOT)
    if hashlib.sha256(tracked_patch).hexdigest() != source_state["tracked_patch_sha256"]:
        raise RuntimeError("tracked source patch changed while creating prepared snapshot")
    (target / "tracked.patch").write_bytes(tracked_patch)
    (target / "untracked-inputs").mkdir()
    for name, digest in source_state["untracked_sha256"].items():
        source = ROOT / name
        if sha(source) != digest:
            raise RuntimeError(f"untracked source changed while sealing snapshot: {name}")
        copy = target / "untracked-inputs" / name
        copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, copy)
    qemu = list(command)
    qemu[qemu.index("-kernel") + 1] = str(target / "kernel.elf")
    drive_index = qemu.index("-drive") + 1
    drive_arg = qemu[drive_index]
    qemu[drive_index] = re.sub(r"file=[^,]+", f"file={target / 'base.img'}", drive_arg, count=1)
    build_text = build_log.read_text(errors="replace")
    compiler_invocation = next((line for line in build_text.splitlines()
                                if " -c " in line and "gcc" in line), None)
    rust_version = None
    if args.task_objects_impl == "rust":
        rust_version = subprocess.run(["rustc", "--version", "--verbose"], text=True,
                                      stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, check=False).stdout
    actual_kernel_build_dir = kernel.parent.resolve()
    actual_user_build_dir = app.parent.resolve()
    manifest = {
        "schema": 1, "arch": arch, "cpus": args.cpus,
        "task_objects_impl": args.task_objects_impl,
        "build": {"abi": "both", "bringup": 0, "nr_cpus": args.cpus,
                  "opt_environment": os.environ.get("OPT", "Makefile default (-O3)"),
                  "command": ["make", f"ARCH={arch}", "ABI=both", "BRINGUP=0",
                              f"NR_CPUS={args.cpus}",
                              f"TASK_OBJECTS_IMPL={args.task_objects_impl}",
                              f"BUILD_DIR={actual_kernel_build_dir}",
                              f"USER_BUILD_DIR={actual_user_build_dir}",
                              f"-j{args.jobs}", "dev-build"]},
        "build_output_dirs": {
            "kernel": str(actual_kernel_build_dir),
            "user": str(actual_user_build_dir),
        },
        "source_inputs_sha256": source_inputs(),
        "source_tree_state": source_state,
        "sealed_source_artifacts_sha256": {
            str(path.relative_to(target)): sha(path)
            for path in [target / "tracked.patch", *sorted(
                (target / "untracked-inputs").rglob("*"))] if path.is_file()
        },
        "artifacts_sha256": {name: sha(target / name) for name in
                             ["kernel.elf", "base.img", "native-futex", "proc/task_object.o",
                              "build.log"]},
        "qemu_argv": qemu,
        "tool_versions": {
            "compiler_invocation": compiler_invocation,
            "rustc_verbose": rust_version,
            "build_log_sha256": sha(build_log),
            "qemu": subprocess.run(["qemu-system-" + arch, "--version"], text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   check=False).stdout.splitlines()[0],
        },
    }
    (target / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return target


def load_snapshot(path, arch, impl, cpus):
    manifest_path = path / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get("schema") != 1 or manifest.get("arch") != arch or
            manifest.get("task_objects_impl") != impl or manifest.get("cpus") != cpus):
        raise RuntimeError("prepared snapshot configuration does not match this run")
    expected = manifest.get("artifacts_sha256", {})
    for name in ["kernel.elf", "base.img", "native-futex", "proc/task_object.o", "build.log"]:
        artifact = path / name
        if not artifact.is_file() or sha(artifact) != expected.get(name):
            raise RuntimeError(f"prepared snapshot artifact hash mismatch: {name}")
    for name, digest in manifest.get("sealed_source_artifacts_sha256", {}).items():
        artifact = path / name
        if not artifact.is_file() or sha(artifact) != digest:
            raise RuntimeError(f"prepared snapshot source evidence hash mismatch: {name}")
    command = manifest.get("qemu_argv")
    if not isinstance(command, list) or "-kernel" not in command or "-drive" not in command:
        raise RuntimeError("prepared snapshot has invalid QEMU argv")
    return manifest


def run_case(arch, args, output):
    native_tag = {"riscv64": "rv", "aarch64": "aarch64", "x86_64": "x86_64"}[arch]
    build = None
    if args.prepared:
        snapshot = (ROOT / args.prepared).resolve()
        manifest = load_snapshot(snapshot, arch, args.task_objects_impl, args.cpus)
        command = list(manifest["qemu_argv"])
        kernel = snapshot / "kernel.elf"
        image = snapshot / "base.img"
        app = snapshot / "native-futex"
        workload_sources = manifest["source_inputs_sha256"]
    else:
        source_state_before = source_tree_state() if args.prepare_to else None
        flags = make_build_flags(arch, args)
        build = ["make", *flags, f"-j{args.jobs}", "dev-build"]
        with (output / f"{arch}-build.log").open("w") as build_log:
            subprocess.run(build, cwd=ROOT, check=True, stdout=build_log,
                           stderr=subprocess.STDOUT)
        command = shlex.split(subprocess.check_output(
            ["make", "-s", *flags, "_qemu_argv"], cwd=ROOT, text=True).strip())
        kernel = ROOT / command[command.index("-kernel") + 1]
        drive = command[command.index("-drive") + 1]
        image = ROOT / drive.split("file=", 1)[1].split(",", 1)[0]
        app = native_app_path(arch, args)
        assert app.is_file(), f"Native fixture ELF missing: {app}"
        workload_sources = source_inputs()
        if args.prepare_to:
            source_state_after = source_tree_state()
            if source_state_after != source_state_before:
                raise RuntimeError("source/worktree state changed during the build; discard this snapshot")
            prepared = prepare_snapshot(arch, args, command, kernel, image, app,
                                        output / f"{arch}-build.log", source_state_before)
            print(f"PREPARED Native sync input: {prepared}", flush=True)
            return None
    # Every prepared run gets its own writable disk image. Never boot the
    # sealed template, and never reuse an existing run image.
    run_image = output / "guest.img"
    if run_image.exists():
        raise RuntimeError(f"refusing to overwrite per-run disk image: {run_image}")
    shutil.copy2(image, run_image)
    drive_index = command.index("-drive") + 1
    command[drive_index] = re.sub(r"file=[^,]+", f"file={run_image}",
                                  command[drive_index], count=1)
    log = output / f"{arch}.log"
    tool_prefix = {"aarch64": "aarch64-linux-gnu-",
                   "riscv64": "riscv64-linux-gnu-",
                   "x86_64": ""}[arch]
    nm = shutil.which(tool_prefix + "nm") or shutil.which("nm") or "nm"
    symbols = subprocess.check_output([nm, "-g", str(kernel)], text=True)
    objdump = (shutil.which(tool_prefix + "objdump")
               or shutil.which("objdump") or "objdump")
    bridge_object = kernel.parent / "proc/task_object.o"
    callsite_disassembly = subprocess.check_output(
        [objdump, "-dr", str(bridge_object)], text=True, errors="replace")
    linked_disassembly = subprocess.check_output(
        [objdump, "-dr", "--disassemble=a20_handle_install_task", str(kernel)],
        text=True, errors="replace")
    if args.task_objects_impl == "rust":
        rust_symbols = ["a20_rust_handle_install_task", "a20_rust_task_object_detach",
                        "a20_rust_task_object_get_task", "a20_rust_task_object_notify",
                        "a20_rust_task_object_put", "a20_rust_task_object_ref",
                        "a20_rust_task_object_serial", "a20_rust_task_object_task_release"]
        assert all(symbol in symbols for symbol in rust_symbols), str(kernel)
        assert "a20_rust_handle_install_task" in callsite_disassembly, str(bridge_object)
        assert "a20_rust_handle_install_task" in linked_disassembly, str(kernel)
    else:
        assert "a20_rust_handle_install_task" not in symbols, str(kernel)
    proof_path = output / f"{arch}-task-object-symbols.txt"
    proof_path.write_text("### C call-site relocations\n" + callsite_disassembly +
                          "\n### Linked kernel wrapper disassembly\n" + linked_disassembly)
    tag = native_tag
    phase = "boot"
    thread_cpus = None
    host_terminated = False
    task_identity_metrics = None
    with log.open("wb") as stream:
        process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE,
                                   stdout=stream, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + args.timeout
        try:
            while time.monotonic() < deadline:
                content = log.read_text(errors="replace")
                assert not re.search(r"PANIC|FATAL|LOCKSTALL|NATIVE_[A-Z_]+: FAIL",
                                     content, re.IGNORECASE), str(log)
                if phase == "boot" and re.search(r"(?:^|[\r\n])# ", content):
                    process.stdin.write(f"/bin/native-futex-{tag}\n".encode())
                    process.stdin.flush()
                    phase = "application"
                    deadline = time.monotonic() + args.timeout
                elif phase == "application" and "NATIVE_FUTEX: PASS" in content:
                    assert all(marker in content for marker in ["NATIVE_COMPUTE_CANCEL: PASS workers=2 exit=137 pure-user=1", "NATIVE_TASK_CONTROL: PASS signal=12 masked=1 exit=137", "NATIVE_SPAWN: PASS children=2 exit=37 args=4 env=2 work=32", "NATIVE_FILES_REOPEN: PASS count=4100", "NATIVE_FILES: PASS bytes=2048 fnv=60f4edc5", "NATIVE_ATOMIC32: PASS", "NATIVE_ALLOCATOR: PASS", "NATIVE_DOMAIN: PASS", "NATIVE_THREAD: PASS", "NATIVE_RUNTIME: PASS", "NATIVE_OBJECT_DIRECTORY: PASS"]), str(log)
                    compare_markers = re.findall(
                        r"^A20_COMPARE_JSON: (.+)$", content, re.MULTILINE)
                    assert len(compare_markers) == 1, str(log)
                    compare = json.loads(compare_markers[0])
                    assert compare == {
                        "schema": 1,
                        "case": "task-identity/live-handle-weak-pin-v1",
                        "contract": {"warmup_calls": 1000, "calls": 100000,
                                     "errors": 0,
                                     "status_hash_fnv1a32": 235836997},
                        "metrics": compare["metrics"],
                    }, str(log)
                    assert (set(compare["metrics"]) ==
                            {"elapsed_ns", "ns_per_transaction"} and
                            compare["metrics"]["elapsed_ns"] > 0 and
                            compare["metrics"]["ns_per_transaction"] > 0), str(log)
                    task_identity_metrics = compare["metrics"]
                    match = re.search(r"NATIVE_THREAD_CPUS: (\d+)->(\d+)", content)
                    assert match, str(log)
                    balance = re.search(r"NATIVE_BALANCE: PASS jobs=32 sum=4359168 mask=([0-9a-f]{8}) range=dynamic", content)
                    assert balance, str(log)
                    balance_mask = int(balance[1], 16)
                    assert balance_mask and not balance_mask & ~((1 << args.cpus) - 1), str(log)
                    assert args.cpus == 1 or balance_mask.bit_count() >= 2, str(log)
                    thread_cpus = [int(match[1]), int(match[2])]
                    assert all(cpu < args.cpus for cpu in thread_cpus), str(log)
                    assert args.cpus == 1 or thread_cpus[0] != thread_cpus[1], str(log)
                    assert f"NATIVE_DOMAIN_CPUS: {args.cpus:02d}\n" in content, str(log)
                    process.stdin.write(b"poweroff\n")
                    process.stdin.flush()
                    phase = "shutdown"
                    deadline = time.monotonic() + args.timeout
                elif phase == "shutdown" and "System is going down for power-off NOW" in content:
                    if re.search(r"\[SD\]\s*audit errors=0", content):
                        phase = "complete"
                        break
                if process.poll() is not None:
                    break
                time.sleep(0.05)
        finally:
            if process.poll() is None:
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    host_terminated = True
                    process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stdin.close()
    assert phase == "complete", f"{arch} stopped at {phase}; see {log}"
    final_log = log.read_text(errors="replace")
    assert re.search(r"\[SD\]\s*audit errors=0", final_log), str(log)
    assert not re.search(r"\[SD\]\s*audit errors=[1-9]", final_log), str(log)
    return dict(arch=arch, cpus=args.cpus, domain_online_verified=args.cpus, thread_cpus_verified=thread_cpus, balance_cpu_mask=balance_mask, balance_jobs=32, balance_sum=4359168, dynamic_work_range_checked=True, native_pure_compute_cancel_checked=True, native_task_control_checked=True, native_file_spawn_checked=True, native_file_io_checked=True, concurrent_file_threads_checked=True, passed=True, build=build, command=command,
                log=artifact_path(log), kernel=artifact_path(kernel),
                kernel_sha256=sha(kernel), app=artifact_path(app),
                app_sha256=sha(app),task_objects_impl=args.task_objects_impl,
                prepared_snapshot=artifact_path(snapshot) if args.prepared else None,
                prepared_manifest_sha256=sha(snapshot / "manifest.json") if args.prepared else None,
                workload_sha256=hashlib.sha256(
                    (workload_sources["user/tests/test_native_futex.c"] +
                     workload_sources["user/tests/native-thread-contract.h"]).encode()
                ).hexdigest(),
                task_identity_case="task-identity/live-handle-weak-pin-v1",
                task_identity_contract={"warmup_calls": 1000, "calls": 100000,
                                        "errors": 0,
                                        "status_hash_fnv1a32": 235836997},
                task_identity_metrics=task_identity_metrics,
                call_site_elf_proof=artifact_path(proof_path),
                base_image=artifact_path(image),
                base_image_sha256=sha(image),
                run_image=artifact_path(run_image),
                run_image_sha256=sha(run_image), returncode=process.returncode,
                shutdown_audit_verified=True, audit_errors=0,
                fatal_log_scan_passed=True,
                termination="host SIGTERM after verified shutdown audit" if host_terminated
                            else "QEMU exited naturally", physical_board=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arches", nargs="+", default=["riscv64", "aarch64", "x86_64"],
                        choices=["riscv64", "aarch64", "x86_64"])
    parser.add_argument("--cpus", type=int, default=2, choices=[1, 2, 4])
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--task-objects-impl", choices=["c", "rust"], default="c")
    parser.add_argument("--prepare-to", type=Path,
                        help="build once and seal kernel/app/disk inputs here; do not boot")
    parser.add_argument("--prepared", type=Path,
                        help="boot a hash-checked sealed input set without rebuilding")
    parser.add_argument("--build-dir", type=Path,
                        help="override Makefile BUILD_DIR for an isolated kernel build")
    parser.add_argument("--user-build-dir", type=Path,
                        help="override Makefile USER_BUILD_DIR for isolated user artifacts")
    parser.add_argument("--output", type=Path,
                        default=Path(".kernel-build/cluster/native-sync"))
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout <= 0:
        parser.error("jobs and timeout must be positive")
    if args.prepare_to and args.prepared:
        parser.error("--prepare-to and --prepared are mutually exclusive")
    for option in ("build_dir", "user_build_dir"):
        value = getattr(args, option)
        if value is not None and not value.is_absolute():
            setattr(args, option, (ROOT / value).resolve())
    if (args.prepare_to or args.prepared) and len(args.arches) != 1:
        parser.error("prepared snapshots require exactly one --arches target")
    output = (ROOT / args.output / args.task_objects_impl).resolve()
    if output.exists() and any(output.iterdir()):
        parser.error(f"refusing to overwrite non-empty evidence directory: {output}")
    output.mkdir(parents=True, exist_ok=True)
    if args.prepare_to:
        run_case(args.arches[0], args, output)
        return
    cases = []
    report = output / "report.json"
    # Publish an incomplete report first, so a failed rerun cannot leave a
    # previous all-green report looking like evidence for this invocation.
    report.write_text(json.dumps(dict(passed=False, cases=cases), indent=2) + "\n")
    for arch in args.arches:
        cases.append(run_case(arch, args, output))
        report.write_text(json.dumps(dict(passed=False, cases=cases), indent=2) + "\n")
        print("PASS Native futex/task-object:", arch,
              f"impl={args.task_objects_impl} cpus={args.cpus}", flush=True)
    report.write_text(json.dumps(dict(passed=True, cases=cases), indent=2) + "\n")


if __name__ == "__main__":
    main()
