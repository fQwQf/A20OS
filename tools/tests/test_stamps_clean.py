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
