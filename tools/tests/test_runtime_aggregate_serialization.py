import re
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
TARGETS = REPO / "tools/targets-build.mk"


def _notparallel_directive() -> str:
    lines = TARGETS.read_text().splitlines()
    for index, line in enumerate(lines):
        if line.startswith(".NOTPARALLEL:"):
            directive = [line]
            while directive[-1].rstrip().endswith("\\"):
                index += 1
                directive.append(lines[index])
            return "\n".join(directive)
    raise AssertionError("targets-build.mk has no .NOTPARALLEL directive")


class RuntimeAggregateSerializationTests(unittest.TestCase):
    def test_shared_image_runtime_aggregates_are_explicitly_serialized(self):
        directive = _notparallel_directive()
        targets = set(re.findall(r"\bcheck-[a-z0-9-]+\b", directive))
        self.assertTrue(
            {
                "check-blocking-point-boundary",
                "check-timeout-ownership-boundary",
                "check-mm-lock-model",
                "check-upgrade-userland-smokes",
            }.issubset(targets),
            f"runtime aggregate missing from .NOTPARALLEL: {targets}",
        )

    def test_make_j_serializes_sibling_runtime_smokes_and_control_overlaps(self):
        # The first stub waits for the second to start. With -j2 and no
        # aggregate-level .NOTPARALLEL, this makes overlap deterministic; with
        # the real directive, the first finishes and clears its active marker
        # before make starts the second. No kernel, image, or QEMU is involved.
        helper = textwrap.dedent(
            """\
            import pathlib, sys, time
            root = pathlib.Path(sys.argv[1])
            role = sys.argv[2]
            active = root / 'active'
            second = root / 'second-started'
            overlap = root / 'overlap'
            if role == 'first':
                active.write_text('running')
                deadline = time.monotonic() + 2.0
                while not second.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                if second.exists() and active.exists():
                    overlap.write_text('yes')
                active.unlink(missing_ok=True)
            else:
                second.write_text('started')
                if active.exists():
                    overlap.write_text('yes')
            """
        )

        def run_case(serialized: bool) -> bool:
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "helper.py").write_text(helper)
                makefile = root / "Makefile"
                prefix = _notparallel_directive() + "\n" if serialized else ""
                makefile.write_text(
                    prefix
                    + textwrap.dedent(
                        f"""\
                        .PHONY: check-blocking-point-boundary first second
                        check-blocking-point-boundary: first second
                        first:
                        \t@{sys.executable} {root / 'helper.py'} {root} first
                        second:
                        \t@{sys.executable} {root / 'helper.py'} {root} second
                        """
                    )
                )
                subprocess.run(
                    ["make", "-j2", "-f", str(makefile), "check-blocking-point-boundary"],
                    check=True,
                    timeout=6,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
                return (root / "overlap").exists()

        self.assertFalse(run_case(serialized=True), "serialized aggregate overlapped siblings")
        self.assertTrue(run_case(serialized=False), "unserialized control did not expose overlap")


if __name__ == "__main__":
    unittest.main()
