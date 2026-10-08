"""Keep package/repository/image prerequisites connected in the Make graph."""

from pathlib import Path
import re
import subprocess
import unittest


REPO = Path(__file__).resolve().parents[2]


class PackageMakeDependencyTests(unittest.TestCase):
    def test_pkg_repo_builds_packages_and_image_builds_repo(self):
        result = subprocess.run(
            ["make", "-pRrq", "ARCH=riscv64", "pkg-repo"],
            cwd=REPO,
            check=False,
            capture_output=True,
            text=True,
        )
        # `-q` returns 1 when the requested target is out of date. The make
        # database is still emitted and contains the resolved dependency graph.
        self.assertIn(result.returncode, (0, 1), result.stderr)
        self.assertRegex(result.stdout, r"(?m)^pkg-repo: pkgs$")
        self.assertRegex(result.stdout, r"(?m)^image-world: pkg-repo(?:\s|$)")
        self.assertRegex(result.stdout, r"(?m)^pkgs: .*kernel\.elf.*pkg-key(?:\s|$)")


if __name__ == "__main__":
    unittest.main()
