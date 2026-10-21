#!/usr/bin/env python3
"""Incremental-build stamps for the user and native trees (was targets-dev.mk).

Both rules were the same hand-rolled staleness decision in shell: compare the
build id in a stamp file, then either the expected binaries are missing or some
source file is newer than the stamp, in which case rebuild and rewrite the stamp.

The `find ... -newer` half is the part worth having in Python.  Written as shell
it was a `find` with a prune list, `-print -quit` and a `grep -q` in a pipeline,
where the interaction between the prune expression and the `-quit` is easy to get
subtly wrong and impossible to see.  Here it is a walk with an explicit skip set.

Only the *decision* and the orchestration move; the sub-makes are still make, so
nothing about how the trees are compiled changes.
"""

from __future__ import annotations

import argparse
import fcntl
import fnmatch
import os
import filecmp
import shutil
import re
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def stamp_id(stamp: Path) -> str:
    try:
        return stamp.read_text().strip()
    except OSError:
        return ""


def any_newer(stamp: Path, roots: list[str], skip: list[str]) -> bool:
    """True if any file under `roots` is newer than the stamp.

    Mirrors `find <roots> -prune... -o -type f -newer <stamp> -print -quit`:
    skip whole directories, and stop at the first hit.  Everything is compared
    as an absolute path, because mixing os.path.relpath against absolute skip
    entries silently never matches once the tree is outside the repo.
    """
    if not stamp.exists():
        return False
    ref = stamp.stat().st_mtime
    # find's -path takes a glob, so these are matched with fnmatch against the
    # absolute directory.  Three kinds of entry appear: '*/.git' must stay
    # unanchored to match at any depth, a relative literal or relative glob is
    # anchored to the repo (os.path.join keeps the wildcard intact), and an
    # absolute path is already anchored.  abspath() on any of them would freeze
    # a wildcard into a literal and silently stop pruning.
    def _pat(s: str) -> str:
        if s.startswith("*") or os.path.isabs(s):
            return s
        return os.path.join(str(REPO), s)

    skip_pats = [_pat(s) for s in skip]

    def skipped(path: str) -> bool:
        ap = os.path.abspath(path)
        return any(fnmatch.fnmatch(ap, p) for p in skip_pats)

    for root in roots:
        base = Path(os.path.abspath(REPO / root))
        if base.is_file():
            if base.stat().st_mtime > ref:
                return True
            continue
        if not base.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            if skipped(dirpath):
                dirnames[:] = []
                continue
            dirnames[:] = [d for d in dirnames
                           if not skipped(os.path.join(dirpath, d))]
            for name in filenames:
                try:
                    if (Path(dirpath) / name).stat().st_mtime > ref:
                        return True
                except OSError:
                    continue
    return False


def submake(args: list[str]) -> int:
    r = subprocess.run(["make", *args], cwd=REPO, check=False)
    return r.returncode


def resolved_user_build_dir(user_build_dir: str) -> Path:
    """Resolve the user output directory exactly as the top-level Makefile does."""
    path = Path(user_build_dir)
    return path if path.is_absolute() else REPO / path


@contextmanager
def user_output_lock(user_build_dir: str):
    """Serialize stamp decisions and writers that share a user output tree.

    The lock is a sibling of ``build/<variant>``, not inside it: a rebuild may
    remove that directory, but must not unlink the inode another process is
    using to coordinate.  Build IDs/options are deliberately absent from the
    lock name because all configurations for one USER_VARIANT write the same
    output root.
    """
    output_dir = REPO / user_build_dir
    lock_dir = output_dir.parent
    lock_dir.mkdir(parents=True, exist_ok=True)
    lock_path = lock_dir / f".{output_dir.name}.a20-build.lock"
    with lock_path.open("a") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def cmd_user(a) -> int:
    if not a.build_id:
        raise SystemExit("error: user stamp needs --build-id")
    with user_output_lock(a.user_build_dir):
        stamp = REPO / a.stamp
        stamp.parent.mkdir(parents=True, exist_ok=True)
        need_build = need_clean = False
        init = REPO / a.user_build_dir / "init"
        mksh = REPO / a.user_build_dir / "mksh"

        # Re-evaluate only after taking the output-root lock. Another make
        # process may have completed this exact build while we were waiting.
        if stamp_id(stamp) != a.build_id:
            need_build = need_clean = True
        elif not os.access(init, os.X_OK) or not os.access(mksh, os.X_OK):
            need_build = True
        elif any_newer(stamp, a.roots.split(), a.skip.split()):
            need_build = True

        if not need_build:
            print(f"[USER] {a.build_id} up to date")
            return 0
        # A clean user rebuild removes the whole shared output directory, and
        # even an incremental rebuild can rewrite native targets.  Invalidate
        # the sibling stamp while holding the same output-root lock so another
        # make process cannot treat deleted native artifacts as current.
        (stamp.parent / ".native-build-id").unlink(missing_ok=True)
        user_build_dir = resolved_user_build_dir(a.user_build_dir)
        common = [f"ARCH={a.arch}", f"NOMMU={a.nommu}",
                  f"OPT={a.user_opt}", f"PROFILE={a.profile}",
                  f"ENVELOPE_ACQUIRE_RACE_TEST={a.envelope_acquire_race_test}",
                  f"SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST="
                  f"{a.syscall_smoke_task_construction_test}",
                  f"TRAP_STORM_TEST={getattr(a, 'trap_storm_test', '0')}",
                  f"BUILD_DIR={user_build_dir}"]
        if need_clean:
            rc = submake(["-C", "user", *common, "clean"])
            if rc != 0:
                return rc
        rc = submake(["-C", "user", *common])
        if rc != 0:
            return rc
        stamp.write_text(a.build_id + "\n")
        return 0


def cmd_native(a) -> int:
    if not a.build_id:
        raise SystemExit("error: native stamp needs --build-id")
    native_rust_peer = str(getattr(a, "native_rust_peer", "0"))
    if native_rust_peer not in ("0", "1"):
        raise SystemExit("error: --native-rust-peer must be 0 or 1")
    native_rust_idl_service = str(getattr(a, "native_rust_idl_service", "0"))
    if native_rust_idl_service not in ("0", "1"):
        raise SystemExit("error: --native-rust-idl-service must be 0 or 1")
    if native_rust_idl_service == "1" and native_rust_peer != "1":
        raise SystemExit("error: Rust IDL service requires --native-rust-peer 1")
    native_rust_echod = str(getattr(a, "native_rust_echod", "0"))
    if native_rust_echod not in ("0", "1"):
        raise SystemExit("error: --native-rust-echod must be 0 or 1")
    if native_rust_echod == "1" and a.arch not in ("riscv64", "riscv32", "aarch64", "x86_64"):
        raise SystemExit("error: Rust echod requires ARCH=riscv64, riscv32, aarch64, or x86_64")
    native_rust_svcmgr = str(getattr(a, "native_rust_svcmgr", "0"))
    if native_rust_svcmgr not in ("0", "1"):
        raise SystemExit("error: --native-rust-svcmgr must be 0 or 1")
    if native_rust_svcmgr == "1" and a.arch not in ("riscv64", "riscv32", "aarch64", "x86_64"):
        raise SystemExit("error: Rust svcmgr requires ARCH=riscv64, riscv32, aarch64, or x86_64")
    native_rust_rtcd = str(getattr(a, "native_rust_rtcd", "0"))
    if native_rust_rtcd not in ("0", "1"):
        raise SystemExit("error: --native-rust-rtcd must be 0 or 1")
    if native_rust_rtcd == "1" and a.arch not in ("riscv64", "aarch64"):
        raise SystemExit("error: Rust rtcd requires ARCH=riscv64 or aarch64 Goldfish RTC")
    native_rust_ufsd = str(getattr(a, "native_rust_ufsd", "0"))
    if native_rust_ufsd not in ("0", "1"):
        raise SystemExit("error: --native-rust-ufsd must be 0 or 1")
    native_rust_ufsd_extent_txn = str(getattr(a, "native_rust_ufsd_ext4_extent_txn", "1"))
    if native_rust_ufsd_extent_txn not in ("0", "1"):
        raise SystemExit("error: --native-rust-ufsd-ext4-extent-txn must be 0 or 1")
    native_rust_ufsd_ext4_orphan = str(getattr(a, "native_rust_ufsd_ext4_orphan", "1"))
    if native_rust_ufsd_ext4_orphan not in ("0", "1"):
        raise SystemExit("error: --native-rust-ufsd-ext4-orphan must be 0 or 1")
    test_close_orphan = str(getattr(a, "native_rust_ufsd_test_close_orphan_write_fail_once", "0"))
    test_close_orphan_after = str(getattr(a, "native_rust_ufsd_test_close_orphan_write_fail_after", "1"))
    test_close_orphan_restore = str(getattr(a, "native_rust_ufsd_test_close_orphan_restore_fail", "0"))
    test_close_token_drop_ack = str(getattr(a, "native_rust_ufsd_test_close_token_drop_ack_once", "0"))
    test_ext4_trunc_free = str(getattr(a, "native_rust_ufsd_test_ext4_trunc_free_fail_once", "0"))
    if test_close_orphan not in ("0", "1"):
        raise SystemExit("error: --native-rust-ufsd-test-close-orphan-write-fail-once must be 0 or 1")
    if test_close_orphan_after not in tuple(str(n) for n in range(1, 17)):
        raise SystemExit("error: --native-rust-ufsd-test-close-orphan-write-fail-after must be in 1..16")
    if test_close_orphan_restore not in ("0", "1"):
        raise SystemExit("error: --native-rust-ufsd-test-close-orphan-restore-fail must be 0 or 1")
    if test_close_orphan_restore == "1" and test_close_orphan != "1":
        raise SystemExit("error: restore-failure injection requires close-orphan fail-once")
    if test_close_orphan_restore == "1" and test_close_orphan_after == "16":
        raise SystemExit("error: restore-failure injection needs a write index below 16")
    if test_close_token_drop_ack not in ("0", "1"):
        raise SystemExit("error: --native-rust-ufsd-test-close-token-drop-ack-once must be 0 or 1")
    if test_ext4_trunc_free not in ("0", "1"):
        raise SystemExit("error: --native-rust-ufsd-test-ext4-trunc-free-fail-once must be 0 or 1")
    if test_ext4_trunc_free == "1" and native_rust_ufsd != "1":
        raise SystemExit("error: ext4 truncate-free failure profile requires --native-rust-ufsd 1")
    if test_ext4_trunc_free == "1" and native_rust_ufsd_extent_txn != "1":
        raise SystemExit("error: ext4 truncate-free failure profile requires Rust extent transaction selection")
    if test_ext4_trunc_free == "1" and (test_close_orphan == "1" or test_close_token_drop_ack == "1"):
        raise SystemExit("error: ext4 truncate-free injection is mutually exclusive with close-orphan and close-token profiles")
    if test_close_token_drop_ack == "1" and native_rust_ufsd != "1":
        raise SystemExit("error: close-token ACK-drop profile requires --native-rust-ufsd 1")
    if test_close_token_drop_ack == "1" and test_close_orphan == "1":
        raise SystemExit("error: close-token ACK-drop and close-orphan profiles are mutually exclusive")
    if native_rust_ufsd == "1" and a.arch != "riscv64":
        raise SystemExit("error: Rust ufsd currently requires ARCH=riscv64")
    user_build_dir = getattr(a, "user_build_dir", str(Path(a.stamp).parent))
    resolved_build_dir = resolved_user_build_dir(user_build_dir)
    rust_peer_binary = getattr(a, "rust_peer_binary", "")
    rust_service_binary = getattr(a, "rust_service_binary", "")
    rust_service_test_binary = getattr(a, "rust_service_test_binary", "")
    peer_path = None
    if rust_peer_binary:
        candidate = Path(rust_peer_binary)
        if not candidate.is_absolute():
            candidate = REPO / candidate
        if candidate.name in ("", ".", ".."):
            raise SystemExit("error: Rust peer cleanup path must name a file")
        # Resolve only the directory. Resolving the final component would
        # follow a symlink and unlink its target instead of the generated
        # directory entry when cleaning a disabled peer.
        peer_path = candidate.parent.resolve() / candidate.name
        if peer_path.parent != resolved_build_dir.resolve():
            raise SystemExit("error: Rust peer cleanup path must be a top-level file in USER_BUILD_DIR")
    def generated_user_binary(raw_path: str, label: str) -> Path | None:
        if not raw_path:
            return None
        candidate = Path(raw_path)
        if not candidate.is_absolute():
            candidate = REPO / candidate
        if candidate.name in ("", ".", ".."):
            raise SystemExit(f"error: {label} cleanup path must name a file")
        # Preserve the final directory entry; if it is a symlink, cleanup must
        # unlink that link rather than follow it to another user's file.
        result = candidate.parent.resolve() / candidate.name
        if result.parent != resolved_build_dir.resolve():
            raise SystemExit(f"error: {label} cleanup path must be a top-level file in USER_BUILD_DIR")
        return result

    service_path = generated_user_binary(rust_service_binary, "Rust IDL service")
    service_test_path = generated_user_binary(rust_service_test_binary, "Rust IDL service test")
    rust_echod_path = generated_user_binary(
        getattr(a, "rust_echod_binary", ""), "Rust echod")
    rust_svcmgr_path = generated_user_binary(
        getattr(a, "rust_svcmgr_binary", ""), "Rust svcmgr")
    rust_rtcd_path = generated_user_binary(
        getattr(a, "rust_rtcd_binary", ""), "Rust rtcd")
    rust_ufsd_path = generated_user_binary(
        getattr(a, "rust_ufsd_binary", ""), "Rust ufsd")
    rust_ufsd_test_close_token_path = generated_user_binary(
        getattr(a, "rust_ufsd_test_close_token_binary", ""),
        "Rust ufsd close-token test")
    rust_ufsd_test_ext4_trunc_path = generated_user_binary(
        getattr(a, "rust_ufsd_test_ext4_trunc_free_bin", ""),
        "Rust ufsd ext4 truncate test")
    with user_output_lock(str(resolved_build_dir)):
        stamp = REPO / a.stamp
        stamp.parent.mkdir(parents=True, exist_ok=True)
        # The same native output directory is shared by C-only and opt-in
        # builds. Include the selector in the cache identity so switching
        # either way always runs the matching recursive native-programs build.
        effective_build_id = (f"{a.build_id}\nNATIVE_RUST_PEER={native_rust_peer}"
                              f"\nNATIVE_RUST_IDL_SERVICE={native_rust_idl_service}"
                              f"\nNATIVE_RUST_ECHOD={native_rust_echod}"
                              f"\nNATIVE_RUST_SVCMGR={native_rust_svcmgr}"
                              f"\nNATIVE_RUST_RTCD={native_rust_rtcd}"
                              f"\nNATIVE_RUST_UFSD={native_rust_ufsd}"
                              f"\nNATIVE_RUST_UFSD_EXT4_ORPHAN={native_rust_ufsd_ext4_orphan}"
                              f"\nNATIVE_RUST_UFSD_EXT4_EXTENT_TXN={native_rust_ufsd_extent_txn}"
                              f"\nNATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE={test_close_orphan}"
                              f"\nNATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER={test_close_orphan_after}")
        effective_build_id += f"\nNATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL={test_close_orphan_restore}"
        effective_build_id += f"\nNATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE={test_close_token_drop_ack}"
        effective_build_id += f"\nNATIVE_RUST_UFSD_TEST_EXT4_TRUNC_FREE_FAIL_ONCE={test_ext4_trunc_free}"
        old_build_id = stamp_id(stamp)
        old_rust_svcmgr = "NATIVE_RUST_SVCMGR=1" in old_build_id.splitlines()
        old_rust_echod = "NATIVE_RUST_ECHOD=1" in old_build_id.splitlines()
        native_tag = {"riscv64": "rv", "riscv32": "rv32",
                      "aarch64": "aarch64", "x86_64": "x86_64"}.get(a.arch)
        echod_binary = (resolved_build_dir / f"svc-echod-{native_tag}"
                        if native_tag is not None else None)
        echod_c_reference = (resolved_build_dir / f"svc-echod-c-{native_tag}"
                             if native_tag is not None else None)
        manager_binary = (resolved_build_dir / f"svcmgr-{native_tag}"
                          if native_tag is not None else None)
        manager_c_reference = (resolved_build_dir / f"svcmgr-c-{native_tag}"
                               if native_tag is not None else None)
        rtcd_binary = (resolved_build_dir / f"rtcd-{native_tag}.a20drv"
                       if a.arch in ("riscv64", "aarch64") and native_tag is not None else None)
        rtcd_c_reference = (resolved_build_dir / f"rtcd-c-{native_tag}.a20drv"
                            if a.arch in ("riscv64", "aarch64") and native_tag is not None else None)
        need_build = False
        if old_build_id != effective_build_id:
            need_build = True
        elif any(not os.access(REPO / b, os.X_OK) for b in a.binaries.split()):
            need_build = True
        elif (native_rust_peer == "0" and peer_path is not None and
              (peer_path.exists() or peer_path.is_symlink())):
            # A direct `native-rust-idl-peer` invocation can create the ELF
            # without changing the C-only stamp. Force this build to clean it
            # and rewrite the stamp so dependent images are rebuilt as well.
            need_build = True
        elif (native_rust_idl_service == "0" and (
                (native_rust_echod == "0" and service_path is not None and
                 (service_path.exists() or service_path.is_symlink())) or
                (service_test_path is not None and
                 (service_test_path.exists() or service_test_path.is_symlink())))):
            # A direct service target can create both top-level artifacts
            # without changing the native stamp. A later disabled dev-build
            # must refresh the image inputs and remove those generated files.
            need_build = True
        elif (native_rust_svcmgr == "0" and rust_svcmgr_path is not None and
              (rust_svcmgr_path.exists() or rust_svcmgr_path.is_symlink())):
            # A direct Rust supervisor target may leave an ELF in the shared
            # image input directory. A disabled dev-build must remove it.
            need_build = True
        elif (native_rust_echod == "0" and rust_echod_path is not None and
              (rust_echod_path.exists() or rust_echod_path.is_symlink())):
            # A direct Rust echod build can leave an ELF in the shared image
            # input directory. A disabled dev-build must remove it.
            need_build = True
        elif (native_rust_rtcd == "0" and rust_rtcd_path is not None and
              (rust_rtcd_path.exists() or rust_rtcd_path.is_symlink())):
            need_build = True
        elif (native_rust_ufsd == "0" and rust_ufsd_path is not None and
              (rust_ufsd_path.exists() or rust_ufsd_path.is_symlink())):
            need_build = True
        elif (test_close_token_drop_ack == "0" and
              rust_ufsd_test_close_token_path is not None and
              (rust_ufsd_test_close_token_path.exists() or
               rust_ufsd_test_close_token_path.is_symlink())):
            # Do not let a previously built fault-injection service remain in
            # the next default FAT image as an unselected test executable.
            need_build = True
        elif (test_ext4_trunc_free == "0" and
              rust_ufsd_test_ext4_trunc_path is not None and
              (rust_ufsd_test_ext4_trunc_path.exists() or
               rust_ufsd_test_ext4_trunc_path.is_symlink())):
            # Keep an injected UFSD ELF out of a later production image.
            need_build = True
        elif any_newer(stamp, a.roots.split(), []):
            need_build = True

        if not need_build:
            print(f"[NATIVE] {a.build_id} up to date")
            return 0
        selector_changed = (old_rust_svcmgr != (native_rust_svcmgr == "1"))
        echod_selector_changed = (old_rust_echod != (native_rust_echod == "1"))
        old_rust_rtcd = "NATIVE_RUST_RTCD=1" in old_build_id.splitlines()
        rtcd_selector_changed = (old_rust_rtcd != (native_rust_rtcd == "1"))
        old_rust_ufsd = "NATIVE_RUST_UFSD=1" in old_build_id.splitlines()
        ufsd_selector_changed = (old_rust_ufsd != (native_rust_ufsd == "1"))
        ufsd_binary = resolved_build_dir / "ufsd-rv" if a.arch == "riscv64" else None
        ufsd_c_reference = resolved_build_dir / "ufsd-c-rv" if a.arch == "riscv64" else None
        if echod_binary is not None and echod_c_reference is not None:
            if echod_c_reference.is_file():
                shutil.copy2(echod_c_reference, echod_binary)
            elif echod_selector_changed:
                echod_binary.unlink(missing_ok=True)
        if manager_binary is not None and manager_c_reference is not None:
            # Seed the normal make target with the preserved C output before
            # submake.  That gives incremental dependency checks a C artifact
            # to compare against, while a C-source change still rebuilds it.
            # When switching from Rust without a reference (for example an
            # interrupted first opt-in build), remove the wrong implementation
            # so make cannot accept it as the requested C target.
            if manager_c_reference.is_file():
                shutil.copy2(manager_c_reference, manager_binary)
            elif selector_changed:
                manager_binary.unlink(missing_ok=True)
        if rtcd_binary is not None and rtcd_c_reference is not None:
            if rtcd_c_reference.is_file():
                shutil.copy2(rtcd_c_reference, rtcd_binary)
            elif rtcd_selector_changed:
                rtcd_binary.unlink(missing_ok=True)
        if ufsd_binary is not None and ufsd_c_reference is not None:
            if ufsd_c_reference.is_file():
                shutil.copy2(ufsd_c_reference, ufsd_binary)
            elif ufsd_selector_changed:
                ufsd_binary.unlink(missing_ok=True)
        rc = submake([f"ARCH={a.arch}", f"NOMMU={a.nommu}", f"OPT={a.opt}",
                      f"NATIVE_RUST_PEER={native_rust_peer}",
                      f"NATIVE_RUST_IDL_SERVICE={native_rust_idl_service}",
                      f"NATIVE_RUST_ECHOD={native_rust_echod}",
                      f"NATIVE_RUST_SVCMGR={native_rust_svcmgr}",
                      f"NATIVE_RUST_RTCD={native_rust_rtcd}",
                      f"NATIVE_RUST_UFSD={native_rust_ufsd}",
                      f"NATIVE_RUST_UFSD_EXT4_ORPHAN={native_rust_ufsd_ext4_orphan}",
                      f"NATIVE_RUST_UFSD_EXT4_EXTENT_TXN={native_rust_ufsd_extent_txn}",
                      f"NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_ONCE={test_close_orphan}",
                      f"NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_WRITE_FAIL_AFTER={test_close_orphan_after}",
                      f"NATIVE_RUST_UFSD_TEST_CLOSE_ORPHAN_RESTORE_FAIL={test_close_orphan_restore}",
                      f"NATIVE_RUST_UFSD_TEST_CLOSE_TOKEN_DROP_ACK_ONCE={test_close_token_drop_ack}",
                      f"NATIVE_RUST_UFSD_TEST_EXT4_TRUNC_FREE_FAIL_ONCE={test_ext4_trunc_free}",
                      f"USER_BUILD_DIR={resolved_build_dir}",
                      "native-programs"])
        if rc != 0:
            return rc
        # The FAT image builder copies every top-level user build file. Remove
        # only this generated opt-in artifact after a successful C-only build;
        # otherwise a 1→0 transition would silently package a stale peer.
        if native_rust_peer == "0" and peer_path is not None:
            peer_path.unlink(missing_ok=True)
        if native_rust_idl_service == "0":
            if service_test_path is not None:
                service_test_path.unlink(missing_ok=True)
            if native_rust_echod == "0" and service_path is not None:
                service_path.unlink(missing_ok=True)
        if native_rust_svcmgr == "0" and rust_svcmgr_path is not None:
            rust_svcmgr_path.unlink(missing_ok=True)
        if native_rust_rtcd == "0" and rust_rtcd_path is not None:
            rust_rtcd_path.unlink(missing_ok=True)
        if native_rust_ufsd == "0" and rust_ufsd_path is not None:
            rust_ufsd_path.unlink(missing_ok=True)
        if test_close_token_drop_ack == "0" and rust_ufsd_test_close_token_path is not None:
            rust_ufsd_test_close_token_path.unlink(missing_ok=True)
        if test_ext4_trunc_free == "0" and rust_ufsd_test_ext4_trunc_path is not None:
            rust_ufsd_test_ext4_trunc_path.unlink(missing_ok=True)
        if manager_binary is not None and manager_c_reference is not None:
            if native_rust_svcmgr == "1":
                # Keep the C implementation available for behavior comparison,
                # but publish Rust at the pathname consumed by smoke scripts
                # and runtime service launchers.  Preserve the last C-linked
                # output across repeated opt-in builds before replacing it.
                shutil.copy2(manager_binary, manager_c_reference)
                if rust_svcmgr_path is None or not rust_svcmgr_path.is_file():
                    raise SystemExit("error: Rust svcmgr build succeeded without its ELF")
                shutil.copy2(rust_svcmgr_path, manager_binary)
            else:
                manager_c_reference.unlink(missing_ok=True)
        if echod_binary is not None and echod_c_reference is not None:
            if native_rust_echod == "1":
                if not echod_binary.is_file():
                    raise SystemExit("error: C echod build succeeded without its runtime ELF")
                shutil.copy2(echod_binary, echod_c_reference)
                if rust_echod_path is None or not rust_echod_path.is_file():
                    raise SystemExit("error: Rust echod build succeeded without its ELF")
                shutil.copy2(rust_echod_path, echod_binary)
            else:
                echod_c_reference.unlink(missing_ok=True)
        if rtcd_binary is not None and rtcd_c_reference is not None:
            if native_rust_rtcd == "1":
                if not rtcd_binary.is_file():
                    raise SystemExit("error: C rtcd build succeeded without its runtime ELF")
                shutil.copy2(rtcd_binary, rtcd_c_reference)
                if rust_rtcd_path is None or not rust_rtcd_path.is_file():
                    raise SystemExit("error: Rust rtcd build succeeded without its ELF")
                shutil.copy2(rust_rtcd_path, rtcd_binary)
            else:
                rtcd_c_reference.unlink(missing_ok=True)
        if ufsd_binary is not None and ufsd_c_reference is not None:
            if native_rust_ufsd == "1":
                if not ufsd_binary.is_file():
                    raise SystemExit("error: C ufsd build succeeded without its runtime ELF")
                if not old_rust_ufsd:
                    shutil.copy2(ufsd_binary, ufsd_c_reference)
                elif not ufsd_c_reference.is_file():
                    raise SystemExit("error: Rust ufsd selector changed without a preserved C reference ELF")
                if rust_ufsd_path is None or not rust_ufsd_path.is_file():
                    raise SystemExit("error: Rust ufsd build succeeded without its ELF")
                shutil.copy2(rust_ufsd_path, ufsd_binary)
            else:
                ufsd_c_reference.unlink(missing_ok=True)
        stamp.write_text(effective_build_id + "\n")
        return 0


def cmd_build_flags(a) -> int:
    """Rewrite the build-flags stamp only when the signature actually changed.

    The stamp is an order-only-ish dependency of every object file, so its mtime
    is load-bearing: touching it on an unchanged signature would force a full
    kernel rebuild on every invocation.  The shell version compared the freshly
    written temp file against the live stamp and discarded the temp on a match;
    byte comparison, not mtime, decided.
    """
    stamp = REPO / a.stamp
    stamp.parent.mkdir(parents=True, exist_ok=True)
    body = a.build_flags_sig + "\n"
    tmp = stamp.with_name(stamp.name + ".tmp")
    tmp.write_text(body, encoding="utf-8")
    if stamp.is_file() and stamp.read_text(encoding="utf-8") == body:
        tmp.unlink()
        return 0
    tmp.replace(stamp)
    return 0


def cmd_clean(a) -> int:
    """Delete kernel object files and build trees.

    Replaces the `find ... -delete` / `rm -rf` pairs that `clean` and
    `_reset_obj` each carried, so the two no longer have to agree on what a
    reset means.  Sub-makes (`make -C user clean`) stay in make: they are
    recursive builds, not file surgery.
    """
    n = 0
    for d in a.find_root:
        root = REPO / d
        if not root.is_dir():
            continue
        for f in root.rglob("*.o"):
            f.unlink()
            n += 1
    for d in a.rm_rf:
        path = REPO / d
        if path.is_symlink() and d == ".kernel-build":
            # Developers may point the build root at a dedicated external
            # cache. Preserve that link, but only empty it when its immediate
            # children look like our generated build directories. This keeps
            # `make clean` from silently retaining stale objects while making
            # an unexpected symlink target a loud, non-destructive error.
            target = path.resolve(strict=True)
            if (not target.is_dir() or target == REPO or REPO in target.parents
                    or target in REPO.parents):
                raise SystemExit(f"error: refusing unsafe .kernel-build target: {target}")
            marker = target / ".a20-build-root"
            if marker.is_symlink() or not marker.is_file() or marker.read_text(encoding="utf-8").strip() != "a20-build-root-v1":
                raise SystemExit(
                    f"error: refusing unmarked .kernel-build target: {target} "
                    "(initialize a dedicated build cache with .a20-build-root containing a20-build-root-v1)"
                )
            allowed = re.compile(
                r"^(?:aarch64|arm32|armv7m|loongarch32|loongarch64|ppc64le|riscv32|riscv64|x86_64)-.*|smoke$"
            )
            children = [p for p in target.iterdir() if p != marker]
            unexpected = [p.name for p in children if not allowed.fullmatch(p.name)]
            if unexpected or any(p.is_symlink() or not p.is_dir() for p in children):
                raise SystemExit(
                    "error: refusing to clear unexpected .kernel-build contents: "
                    + ", ".join(unexpected or [p.name for p in children if p.is_symlink() or not p.is_dir()])
                )
            for child in children:
                shutil.rmtree(child)
        elif path.is_symlink():
            # rmtree does not remove directory symlinks; unlink explicitly so
            # other rm-rf callers do not leave stale links behind.
            path.unlink()
        else:
            shutil.rmtree(path, ignore_errors=True)
    for f in a.rm_f:
        (REPO / f).unlink(missing_ok=True)
    print(f"[CLEAN] {n} object file(s) removed")
    return 0


def cmd_adopt(a) -> int:
    """`cmp -s TMP TARGET && rm -f TMP || mv -f TMP TARGET`.

    Shared by the generated config headers, whose *content* is still produced
    in make but whose write-if-changed decision is not.  Byte comparison, not
    mtime, decides -- these headers are prerequisites of the objects that
    include them, so touching an unchanged header would force a rebuild.
    """
    tmp, target = REPO / a.tmp, REPO / a.target
    if target.is_file() and tmp.is_file() and \
            filecmp.cmp(tmp, target, shallow=False):
        tmp.unlink()
        return 0
    tmp.replace(target)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("user", "native"):
        s = sub.add_parser(name)
        s.add_argument("--stamp", required=True)
        s.add_argument("--build-id", required=True)
        s.add_argument("--arch", required=True)
        s.add_argument("--nommu", default="0")
        s.add_argument("--opt", default="")
        s.add_argument("--roots", default="")
        s.add_argument("--skip", default="")
        if name == "user":
            s.add_argument("--user-build-dir", required=True)
            s.add_argument("--user-opt", default="")
            s.add_argument("--profile", default="")
            s.add_argument("--envelope-acquire-race-test", choices=("0", "1"), default="0")
            s.add_argument("--syscall-smoke-task-construction-test",
                           choices=("0", "1"), default="0")
            s.add_argument("--trap-storm-test",
                           choices=("0", "1"), default="0")
            s.add_argument("--user-variant", required=True)
        else:
            s.add_argument("--user-build-dir")
            s.add_argument("--binaries", default="")
            s.add_argument("--native-rust-peer", choices=("0", "1"), default="0")
            s.add_argument("--rust-peer-binary", default="")
            s.add_argument("--native-rust-idl-service", choices=("0", "1"), default="0")
            s.add_argument("--rust-service-binary", default="")
            s.add_argument("--rust-service-test-binary", default="")
            s.add_argument("--native-rust-echod", choices=("0", "1"), default="0")
            s.add_argument("--rust-echod-binary", default="")
            s.add_argument("--native-rust-svcmgr", choices=("0", "1"), default="0")
            s.add_argument("--rust-svcmgr-binary", default="")
            s.add_argument("--native-rust-rtcd", choices=("0", "1"), default="0")
            s.add_argument("--rust-rtcd-binary", default="")
            s.add_argument("--native-rust-ufsd", choices=("0", "1"), default="0")
            s.add_argument("--native-rust-ufsd-ext4-orphan", choices=("0", "1"), default="1")
            s.add_argument("--native-rust-ufsd-ext4-extent-txn", choices=("0", "1"), default="1")
            s.add_argument("--native-rust-ufsd-test-ext4-trunc-free-fail-once",
                           choices=("0", "1"), default="0")
            s.add_argument("--native-rust-ufsd-test-close-orphan-write-fail-once",
                           choices=("0", "1"), default="0")
            s.add_argument("--native-rust-ufsd-test-close-orphan-write-fail-after",
                           choices=tuple(str(n) for n in range(1, 17)), default="1")
            s.add_argument("--native-rust-ufsd-test-close-orphan-restore-fail",
                           choices=("0", "1"), default="0")
            s.add_argument("--native-rust-ufsd-test-close-token-drop-ack-once",
                           choices=("0", "1"), default="0")
            s.add_argument("--rust-ufsd-binary", default="")
            s.add_argument("--rust-ufsd-test-close-token-binary", default="")
            s.add_argument("--rust-ufsd-test-ext4-trunc-free-bin", default="")
    s = sub.add_parser("clean")
    s.add_argument("--find-root", action="append", default=[])
    s.add_argument("--rm-rf", action="append", default=[])
    s.add_argument("--rm-f", action="append", default=[])
    s = sub.add_parser("adopt")
    s.add_argument("--tmp", required=True)
    s.add_argument("--target", required=True)
    s = sub.add_parser("build-flags")
    s.add_argument("--stamp", required=True)
    s.add_argument("--build-flags-sig", required=True)
    a = ap.parse_args()
    return {"user": cmd_user, "native": cmd_native,
            "build-flags": cmd_build_flags,
            "clean": cmd_clean,
            "adopt": cmd_adopt}[a.cmd](a)


if __name__ == "__main__":
    raise SystemExit(main())
