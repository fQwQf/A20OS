import argparse
import importlib.util
import multiprocessing
import os
import shutil
import tempfile
import time
import unittest
from pathlib import Path


STAMPS = Path(__file__).resolve().parents[1] / "stamps.py"
spec = importlib.util.spec_from_file_location("a20_stamps", STAMPS)
stamps = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(stamps)


def _parallel_stamp_user(repo: str, log: str, barrier):
    stamps.REPO = Path(repo)

    def fake_submake(args):
        with open(log, "a", encoding="utf-8") as stream:
            stream.write("clean\n" if args[-1] == "clean" else "build\n")
        out = Path(repo) / "user/build/riscv64"
        if args[-1] == "clean":
            shutil.rmtree(out, ignore_errors=True)
        else:
            out.mkdir(parents=True, exist_ok=True)
            for name in ("init", "mksh"):
                path = out / name
                path.write_text("binary")
                path.chmod(0o755)
        return 0

    stamps.submake = fake_submake
    args = argparse.Namespace(
        build_id="same-user-build", stamp="user/build/riscv64/.build-id",
        user_build_dir="user/build/riscv64", arch="riscv64", nommu="0",
        user_opt="-O3", profile="full", user_variant="riscv64",
        roots="", skip="",
    )
    barrier.wait()
    raise SystemExit(stamps.cmd_user(args))


def _different_id_user(repo: str, log: str, build_id: str, ready, release=None):
    stamps.REPO = Path(repo)

    def fake_submake(args):
        command = "clean" if args[-1] == "clean" else "build"
        with open(log, "a", encoding="utf-8") as stream:
            stream.write(f"{build_id}-{command}-start\n")
        if command == "clean":
            shutil.rmtree(Path(repo) / "user/build/riscv64", ignore_errors=True)
        else:
            out = Path(repo) / "user/build/riscv64"
            out.mkdir(parents=True, exist_ok=True)
            for name in ("init", "mksh"):
                path = out / name
                path.write_text("binary")
                path.chmod(0o755)
        if release is not None and command == "clean":
            ready.put("first-holds-lock")
            release.wait(5)
        with open(log, "a", encoding="utf-8") as stream:
            stream.write(f"{build_id}-{command}-end\n")
        return 0

    stamps.submake = fake_submake
    args = argparse.Namespace(
        build_id=build_id, stamp="user/build/riscv64/.build-id",
        user_build_dir="user/build/riscv64", arch="riscv64", nommu="0",
        user_opt="-O3", profile="full", user_variant="riscv64",
        roots="", skip="",
    )
    if release is None:
        ready.put("second-ready")
    raise SystemExit(stamps.cmd_user(args))


def _parallel_stamp_native(repo: str, log: str, barrier):
    stamps.REPO = Path(repo)

    def fake_submake(args):
        with open(log, "a", encoding="utf-8") as stream:
            stream.write("native-start\n")
        out = Path(repo) / "user/build/riscv64/native-tool"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("binary")
        out.chmod(0o755)
        time.sleep(0.1)
        with open(log, "a", encoding="utf-8") as stream:
            stream.write("native-end\n")
        return 0

    stamps.submake = fake_submake
    barrier.wait()
    args = argparse.Namespace(
        build_id="native-build", stamp="user/build/riscv64/.native-build-id",
        binaries="user/build/riscv64/native-tool", arch="riscv64", nommu="0",
        opt="-O3", roots="",
    )
    raise SystemExit(stamps.cmd_native(args))


def _hold_user_root_lock(repo: str, variant: str, acquired, release=None):
    stamps.REPO = Path(repo)
    with stamps.user_output_lock(f"user/build/{variant}"):
        acquired.put(variant)
        if release is not None:
            if not release.wait(5):
                raise RuntimeError("timed out waiting to release user-root lock")


def _parallel_stamp_user_with_barrier(repo: str, log: str, barrier):
    stamps.REPO = Path(repo)

    def fake_submake(args):
        command = "clean" if args[-1] == "clean" else "user"
        with open(log, "a", encoding="utf-8") as stream:
            stream.write(f"{command}-start\n")
        out = Path(repo) / "user/build/riscv64"
        if command == "clean":
            shutil.rmtree(out, ignore_errors=True)
        else:
            out.mkdir(parents=True, exist_ok=True)
            for name in ("init", "mksh"):
                path = out / name
                path.write_text("binary")
                path.chmod(0o755)
        time.sleep(0.1)
        with open(log, "a", encoding="utf-8") as stream:
            stream.write(f"{command}-end\n")
        return 0

    stamps.submake = fake_submake
    barrier.wait()
    args = argparse.Namespace(
        build_id="same-user-build", stamp="user/build/riscv64/.build-id",
        user_build_dir="user/build/riscv64", arch="riscv64", nommu="0",
        user_opt="-O3", profile="full", user_variant="riscv64",
        roots="", skip="",
    )
    raise SystemExit(stamps.cmd_user(args))


class CleanSymlinkTests(unittest.TestCase):
    def test_clean_empties_build_target_and_preserves_link(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            target = base / "cache"
            repo.mkdir()
            target.mkdir()
            (target / ".a20-build-root").write_text("a20-build-root-v1\n")
            (target / "riscv64-qemu-virt-riscv64-both-dev-preempt").mkdir()
            (target / "riscv64-qemu-virt-riscv64-both-dev-preempt" / "kernel.o").write_text("x")
            link = repo / ".kernel-build"
            link.symlink_to(target, target_is_directory=True)
            old_repo = stamps.REPO
            stamps.REPO = repo
            try:
                stamps.cmd_clean(argparse.Namespace(find_root=[], rm_rf=[".kernel-build"], rm_f=[]))
            finally:
                stamps.REPO = old_repo
            self.assertTrue(link.is_symlink())
            self.assertEqual([p.name for p in target.iterdir()], [".a20-build-root"])

    def test_clean_accepts_loongarch32_build_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            target = base / "cache"
            repo.mkdir()
            target.mkdir()
            (target / ".a20-build-root").write_text("a20-build-root-v1\n")
            (target / "loongarch32-nailoong-bringup").mkdir(parents=True)
            link = repo / ".kernel-build"
            link.symlink_to(target, target_is_directory=True)
            old_repo = stamps.REPO
            stamps.REPO = repo
            try:
                stamps.cmd_clean(argparse.Namespace(find_root=[], rm_rf=[".kernel-build"], rm_f=[]))
            finally:
                stamps.REPO = old_repo
            self.assertTrue(link.is_symlink())
            self.assertEqual([p.name for p in target.iterdir()], [".a20-build-root"])

    def test_unexpected_target_content_is_left_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            target = base / "cache"
            repo.mkdir()
            target.mkdir()
            (target / ".a20-build-root").write_text("a20-build-root-v1\n")
            marker = target / "keep-me"
            marker.write_text("important")
            link = repo / ".kernel-build"
            link.symlink_to(target, target_is_directory=True)
            old_repo = stamps.REPO
            stamps.REPO = repo
            try:
                with self.assertRaises(SystemExit):
                    stamps.cmd_clean(argparse.Namespace(find_root=[], rm_rf=[".kernel-build"], rm_f=[]))
            finally:
                stamps.REPO = old_repo
            self.assertEqual(marker.read_text(), "important")
            self.assertTrue(link.is_symlink())


class UserBuildLockTests(unittest.TestCase):
    def test_user_submake_builds_only_in_requested_output_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            out = repo / "isolated" / "riscv64-user"
            default = repo / "user/build/riscv64"
            default.mkdir(parents=True)
            marker = default / "keep-me"
            marker.write_text("untouched")
            old_repo, old_submake = stamps.REPO, stamps.submake
            stamps.REPO = repo
            calls = []

            def fake_submake(args):
                calls.append(args)
                build_dir = Path(next(x.split("=", 1)[1] for x in args
                                      if x.startswith("BUILD_DIR=")))
                if args[-1] == "clean":
                    shutil.rmtree(build_dir, ignore_errors=True)
                else:
                    build_dir.mkdir(parents=True, exist_ok=True)
                    for name in ("init", "mksh"):
                        path = build_dir / name
                        path.write_text("binary")
                        path.chmod(0o755)
                return 0

            stamps.submake = fake_submake
            try:
                args = argparse.Namespace(
                    build_id="isolated-user-build", stamp=str(out / ".build-id"),
                    user_build_dir=str(out), arch="riscv64", nommu="0",
                    user_opt="-O3", profile="full", user_variant="riscv64",
                    roots="", skip="",
                )
                self.assertEqual(stamps.cmd_user(args), 0)
            finally:
                stamps.REPO, stamps.submake = old_repo, old_submake

            self.assertEqual(calls[0][-1], "clean")
            self.assertNotIn("clean", calls[1])
            self.assertTrue(all(f"BUILD_DIR={out}" in args for args in calls))
            self.assertTrue((out / "init").is_file())
            self.assertTrue((out / "mksh").is_file())
            self.assertEqual(marker.read_text(), "untouched")

    def test_native_submake_builds_only_in_requested_output_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            out = repo / "isolated" / "riscv64-user"
            default = repo / "user/build/riscv64"
            default.mkdir(parents=True)
            marker = default / "keep-me"
            marker.write_text("untouched")
            binary = out / "native-tool"
            old_repo, old_submake = stamps.REPO, stamps.submake
            stamps.REPO = repo
            calls = []

            def fake_submake(args):
                calls.append(args)
                build_dir = Path(next(x.split("=", 1)[1] for x in args
                                      if x.startswith("USER_BUILD_DIR=")))
                build_dir.mkdir(parents=True, exist_ok=True)
                binary_path = build_dir / "native-tool"
                binary_path.write_text("binary")
                binary_path.chmod(0o755)
                return 0

            stamps.submake = fake_submake
            try:
                args = argparse.Namespace(
                    build_id="isolated-native-build",
                    stamp=str(out / ".native-build-id"), binaries=str(binary),
                    user_build_dir=str(out), arch="riscv64", nommu="0",
                    opt="-O3", roots="",
                )
                self.assertEqual(stamps.cmd_native(args), 0)
            finally:
                stamps.REPO, stamps.submake = old_repo, old_submake

            self.assertEqual(len(calls), 1)
            self.assertIn(f"USER_BUILD_DIR={out}", calls[0])
            self.assertTrue(os.access(binary, os.X_OK))
            self.assertEqual(marker.read_text(), "untouched")

    def test_native_peer_selector_invalidates_cache_and_removes_stale_image_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            out = repo / "isolated" / "riscv64-user"
            default = repo / "user" / "build" / "riscv64"
            default.mkdir(parents=True)
            marker = default / "keep-me"
            marker.write_text("untouched")
            out.mkdir(parents=True)
            native = out / "native-tool"
            peer = out / "rust-idl-peer-riscv64"
            stamp = out / ".native-build-id"
            old_repo, old_submake = stamps.REPO, stamps.submake
            stamps.REPO = repo
            calls = []

            def fake_submake(args):
                calls.append(args)
                build_dir = Path(next(x.split("=", 1)[1] for x in args
                                      if x.startswith("USER_BUILD_DIR=")))
                build_dir.mkdir(parents=True, exist_ok=True)
                binary = build_dir / "native-tool"
                binary.write_text("native")
                binary.chmod(0o755)
                selected_peer = next(x.split("=", 1)[1] for x in args
                                     if x.startswith("NATIVE_RUST_PEER="))
                if selected_peer == "1":
                    peer.write_text("peer")
                    peer.chmod(0o755)
                return 0

            stamps.submake = fake_submake
            try:
                args = argparse.Namespace(
                    build_id="same-user-build", stamp=str(stamp),
                    binaries=str(native), user_build_dir=str(out),
                    arch="riscv64", nommu="0", opt="-O3", roots="",
                    native_rust_peer="0", rust_peer_binary=str(peer),
                )
                for enabled in ("0", "1", "0", "0"):
                    args.native_rust_peer = enabled
                    self.assertEqual(stamps.cmd_native(args), 0)
                # A direct opt-in target can create the peer after a C-only
                # stamp was written. Disabled dev-build must evict it and
                # refresh the stamp-dependent image even though the C cache
                # would otherwise be current.
                peer.write_text("direct peer target")
                peer.chmod(0o755)
                self.assertEqual(stamps.cmd_native(args), 0)
            finally:
                stamps.REPO, stamps.submake = old_repo, old_submake

            self.assertEqual(len(calls), 4)
            self.assertEqual([next(x.split("=", 1)[1] for x in c
                                   if x.startswith("NATIVE_RUST_PEER="))
                              for c in calls], ["0", "1", "0", "0"])
            self.assertTrue(all(c[-1] == "native-programs" for c in calls))
            self.assertFalse(peer.exists(), "disabled image must not retain peer ELF")
            self.assertTrue(os.access(native, os.X_OK))
            self.assertEqual(marker.read_text(), "untouched")
            self.assertIn("NATIVE_RUST_PEER=0", stamp.read_text())

            dev_recipe = (STAMPS.parent / "targets-dev.mk").read_text()
            self.assertIn('--native-rust-peer "$(NATIVE_RUST_PEER)"', dev_recipe)
            self.assertIn('--rust-peer-binary "$(RUST_USER_PEER_BIN)"', dev_recipe)

    def test_native_rust_peer_binary_cleanup_is_confined_to_user_build_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            out = repo / "user-build"
            out.mkdir()
            outside = repo / "keep-this"
            outside.write_text("not a generated user binary")
            binary = out / "native-tool"
            binary.write_text("old")
            binary.chmod(0o755)
            old_repo, old_submake = stamps.REPO, stamps.submake
            stamps.REPO = repo
            stamps.submake = lambda args: 0
            try:
                args = argparse.Namespace(
                    build_id="build", stamp=str(out / ".native-build-id"),
                    binaries=str(binary), user_build_dir=str(out),
                    arch="riscv64", nommu="0", opt="-O3", roots="",
                    native_rust_peer="0", rust_peer_binary=str(outside),
                )
                with self.assertRaises(SystemExit):
                    stamps.cmd_native(args)
            finally:
                stamps.REPO, stamps.submake = old_repo, old_submake
            self.assertEqual(outside.read_text(), "not a generated user binary")

    def test_native_peer_cleanup_unlinks_only_symlink_entry(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            out = repo / "user-build"
            out.mkdir()
            target = repo / "preserve-target"
            target.write_text("keep")
            peer = out / "rust-idl-peer-rv"
            peer.symlink_to(target)
            binary = out / "native-tool"
            binary.write_text("native")
            binary.chmod(0o755)
            old_repo, old_submake = stamps.REPO, stamps.submake
            stamps.REPO = repo
            stamps.submake = lambda args: 0
            try:
                args = argparse.Namespace(
                    build_id="build", stamp=str(out / ".native-build-id"),
                    binaries=str(binary), user_build_dir=str(out),
                    arch="riscv64", nommu="0", opt="-O3", roots="",
                    native_rust_peer="0", rust_peer_binary=str(peer),
                )
                self.assertEqual(stamps.cmd_native(args), 0)
            finally:
                stamps.REPO, stamps.submake = old_repo, old_submake
            self.assertFalse(peer.is_symlink())
            self.assertEqual(target.read_text(), "keep")

    def test_native_uapi_header_change_rebuilds_same_build_id(self):
        targets = (STAMPS.parent / "targets-dev.mk").read_text()
        recipe = targets.split("--roots \"", 2)[2].split('"', 1)[0]
        roots = recipe.split()
        self.assertIn("kernel/include/uapi/a20", roots)
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            out = repo / "user/build/riscv64"
            out.mkdir(parents=True)
            binary = out / "native-tool"
            binary.write_text("old binary")
            binary.chmod(0o755)
            stamp = out / ".native-build-id"
            stamp.write_text("same-id\n")
            header = repo / "kernel/include/uapi/a20/cluster_gossip.h"
            header.parent.mkdir(parents=True)
            header.write_text("new shared protocol\n")
            os.utime(stamp, (1, 1))
            os.utime(header, (2, 2))
            old_repo, old_submake = stamps.REPO, stamps.submake
            calls = []
            stamps.REPO = repo
            stamps.submake = lambda args: calls.append(args) or 0
            try:
                args = argparse.Namespace(
                    build_id="same-id", stamp=str(stamp.relative_to(repo)),
                    binaries=str(binary.relative_to(repo)), arch="riscv64",
                    nommu="0", opt="-O3", roots=" ".join(roots))
                self.assertEqual(stamps.cmd_native(args), 0)
                self.assertEqual(len(calls), 1)
                self.assertEqual(calls[0][-1], "native-programs")
                self.assertEqual(stamps.cmd_native(args), 0)
                self.assertEqual(len(calls), 1)
            finally:
                stamps.REPO, stamps.submake = old_repo, old_submake

    def test_different_arch_output_roots_are_not_serialized(self):
        with tempfile.TemporaryDirectory() as tmp:
            ctx = multiprocessing.get_context("fork")
            acquired = ctx.Queue()
            release = ctx.Event()
            riscv = ctx.Process(target=_hold_user_root_lock,
                                args=(tmp, "riscv64", acquired, release))
            riscv.start()
            self.assertEqual(acquired.get(timeout=5), "riscv64")
            loongarch = ctx.Process(target=_hold_user_root_lock,
                                    args=(tmp, "loongarch64", acquired))
            loongarch.start()
            try:
                # The first process still owns its lock: a second architecture
                # must nevertheless enter its independent output root.
                self.assertEqual(acquired.get(timeout=2), "loongarch64")
            finally:
                release.set()
                riscv.join(5)
                loongarch.join(5)
            self.assertEqual(riscv.exitcode, 0)
            self.assertEqual(loongarch.exitcode, 0)

    def test_arch_user_matrix_targets_use_stamp_locked_build_path(self):
        targets = (Path(__file__).resolve().parents[1] / "targets-build.mk").read_text(
            encoding="utf-8")
        self.assertIn("check-user-build-stamp: $(USER_BUILD_STAMP)", targets)
        for arch in ("riscv64", "loongarch64", "aarch64", "x86_64",
                     "arm32", "riscv32"):
            marker = f"check-{arch}-user:\n"
            self.assertIn(marker, targets)
            recipe = targets.split(marker, 1)[1].split("\n\n", 1)[0]
            self.assertIn(f"ARCH={arch}", recipe)
            self.assertIn("check-user-build-stamp", recipe)
            self.assertNotIn("-C user", recipe)

    def test_parallel_stamp_checks_recheck_after_shared_root_lock(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            log = repo / "commands.log"
            ctx = multiprocessing.get_context("fork")
            barrier = ctx.Barrier(2)
            workers = [ctx.Process(target=_parallel_stamp_user,
                                   args=(str(repo), str(log), barrier))
                       for _ in range(2)]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join(5)
                self.assertEqual(worker.exitcode, 0)
            self.assertEqual(log.read_text().splitlines(), ["clean", "build"])
            self.assertEqual((repo / "user/build/riscv64/.build-id").read_text(),
                             "same-user-build\n")
            self.assertTrue((repo / "user/build/.riscv64.a20-build.lock").is_file())

    def test_different_build_ids_share_one_output_lock(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            log = repo / "commands.log"
            ctx = multiprocessing.get_context("fork")
            ready = ctx.Queue()
            release = ctx.Event()
            first = ctx.Process(target=_different_id_user,
                                args=(str(repo), str(log), "id-a", ready, release))
            first.start()
            self.assertEqual(ready.get(timeout=5), "first-holds-lock")
            second_ready = ctx.Queue()
            second = ctx.Process(target=_different_id_user,
                                 args=(str(repo), str(log), "id-b", second_ready))
            second.start()
            # The second worker has reached cmd_user while the first still
            # holds the output-root lock inside its clean recipe.
            self.assertEqual(second_ready.get(timeout=5), "second-ready")
            release.set()
            first.join(5)
            second.join(5)
            self.assertEqual(first.exitcode, 0)
            self.assertEqual(second.exitcode, 0)
            self.assertEqual(log.read_text().splitlines(), [
                "id-a-clean-start", "id-a-clean-end", "id-a-build-start",
                "id-a-build-end", "id-b-clean-start", "id-b-clean-end",
                "id-b-build-start", "id-b-build-end",
            ])
            self.assertEqual((repo / "user/build/riscv64/.build-id").read_text(),
                             "id-b\n")

    def test_user_clean_invalidates_native_stamp(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            out = repo / "user/build/riscv64"
            out.mkdir(parents=True)
            native_stamp = out / ".native-build-id"
            native_stamp.write_text("old-native-build\n")
            stamps.REPO = repo
            calls = []

            def fake_submake(args):
                command = "clean" if args[-1] == "clean" else "build"
                calls.append(command)
                if command == "clean":
                    shutil.rmtree(out, ignore_errors=True)
                else:
                    out.mkdir(parents=True, exist_ok=True)
                    for name in ("init", "mksh"):
                        path = out / name
                        path.write_text("binary")
                        path.chmod(0o755)
                return 0

            old_submake = stamps.submake
            stamps.submake = fake_submake
            try:
                args = argparse.Namespace(
                    build_id="new-user-build", stamp="user/build/riscv64/.build-id",
                    user_build_dir="user/build/riscv64", arch="riscv64", nommu="0",
                    user_opt="-O3", profile="full", user_variant="riscv64",
                    roots="", skip="",
                )
                self.assertEqual(stamps.cmd_user(args), 0)
            finally:
                stamps.submake = old_submake
            self.assertEqual(calls, ["clean", "build"])
            self.assertFalse(native_stamp.exists())

    def test_user_and_native_stamps_share_the_output_root_lock(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            log = repo / "commands.log"
            ctx = multiprocessing.get_context("fork")
            barrier = ctx.Barrier(2)
            user = ctx.Process(target=_parallel_stamp_user_with_barrier,
                               args=(str(repo), str(log), barrier))
            native = ctx.Process(target=_parallel_stamp_native,
                                 args=(str(repo), str(log), barrier))
            user.start()
            native.start()
            user.join(5)
            native.join(5)
            self.assertEqual(user.exitcode, 0)
            self.assertEqual(native.exitcode, 0)
            events = log.read_text().splitlines()
            # Each stamp handler holds one lock across clean/build and the
            # stamp update, so another stamp writer cannot enter mid-command.
            self.assertIn(events, [
                ["clean-start", "clean-end", "user-start", "user-end",
                 "native-start", "native-end"],
                ["native-start", "native-end", "clean-start", "clean-end",
                 "user-start", "user-end"],
            ])
            native_stamp = repo / "user/build/riscv64/.native-build-id"
            native_binary = repo / "user/build/riscv64/native-tool"
            if native_stamp.exists():
                self.assertTrue(os.access(native_binary, os.X_OK))
            else:
                self.assertFalse(native_binary.exists())


class CleanSafetyTests(unittest.TestCase):
    def test_clean_refuses_unmarked_target_even_with_legal_build_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            target = base / "cache"
            repo.mkdir()
            build = target / "riscv64-qemu-virt-dev"
            build.mkdir(parents=True)
            marker = build / "keep-me"
            marker.write_text("important")
            link = repo / ".kernel-build"
            link.symlink_to(target, target_is_directory=True)
            old_repo = stamps.REPO
            stamps.REPO = repo
            try:
                with self.assertRaisesRegex(SystemExit, "unmarked"):
                    stamps.cmd_clean(argparse.Namespace(find_root=[], rm_rf=[".kernel-build"], rm_f=[]))
            finally:
                stamps.REPO = old_repo
            self.assertEqual(marker.read_text(), "important")
            self.assertTrue(link.is_symlink())

    def test_clean_refuses_repository_parent_even_with_build_named_children(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            repo.mkdir()
            (base / ".a20-build-root").write_text("a20-build-root-v1\n")
            build = base / "riscv64-qemu-virt-dev"
            build.mkdir()
            marker = build / "keep-me"
            marker.write_text("important")
            link = repo / ".kernel-build"
            link.symlink_to(base, target_is_directory=True)
            old_repo = stamps.REPO
            stamps.REPO = repo
            try:
                with self.assertRaises(SystemExit):
                    stamps.cmd_clean(argparse.Namespace(find_root=[], rm_rf=[".kernel-build"], rm_f=[]))
            finally:
                stamps.REPO = old_repo
            self.assertEqual(marker.read_text(), "important")
            self.assertTrue(link.is_symlink())


if __name__ == "__main__":
    unittest.main()
