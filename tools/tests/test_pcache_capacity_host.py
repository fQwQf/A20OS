"""Check cache-capacity policy and C/Rust layout constants on the host."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MIRRORED_CONSTANTS = (
    "PCACHE_MIN_PAGES",
    "PCACHE_MAX_PAGES",
    "PCACHE_TARGET_FRAME_DIVISOR",
    "PCACHE_HASH_BUCKETS",
)


def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected exactly one {signature!r}")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError(f"unterminated {signature!r}")


def read_constants():
    header = (ROOT / "kernel/include/fs/block_cache.h").read_text()
    rust = (ROOT / "kernel/fs/block_cache_state.rs").read_text()
    c_values = {
        name: int(re.search(rf"#define\s+{name}\s+(\d+)", header).group(1))
        for name in MIRRORED_CONSTANTS
    }
    rust_values = {
        name: int(re.search(rf"pub const {name}: usize = (\d+);", rust).group(1))
        for name in MIRRORED_CONSTANTS
    }
    return header, rust, c_values, rust_values


class PcacheCapacityHostTests(unittest.TestCase):
    def test_c_rust_and_differential_hash_constants_match(self):
        _, _, c_values, rust_values = read_constants()
        self.assertEqual(c_values, rust_values)

        differential = (ROOT / "tools/tests/block_cache_state_host.c").read_text()
        c_hash_mirror = int(re.search(r"#define PHASH\s+(\d+)", differential).group(1))
        self.assertEqual(c_hash_mirror, c_values["PCACHE_HASH_BUCKETS"])

    def test_capacity_policy_boundaries(self):
        rustc = shutil.which("rustc")
        if not rustc:
            self.skipTest("rustc is required for the host policy fixture")

        _, source, _, values = read_constants()
        constants = "\n".join(
            f"const {name}: usize = {value};" for name, value in values.items()
        )
        function = extract_function(
            source, 'pub extern "C" fn a20_pcache_page_limit('
        )
        test_module = r'''
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lower_and_upper_boundaries_and_large_ram() {
        assert_eq!(a20_pcache_page_limit(0), PCACHE_MIN_PAGES);
        assert_eq!(
            a20_pcache_page_limit(PCACHE_MIN_PAGES * PCACHE_TARGET_FRAME_DIVISOR - 1),
            PCACHE_MIN_PAGES,
        );
        assert_eq!(
            a20_pcache_page_limit(PCACHE_MIN_PAGES * PCACHE_TARGET_FRAME_DIVISOR),
            PCACHE_MIN_PAGES,
        );
        assert_eq!(
            a20_pcache_page_limit((PCACHE_MAX_PAGES - 1) * PCACHE_TARGET_FRAME_DIVISOR),
            PCACHE_MAX_PAGES - 1,
        );
        assert_eq!(
            a20_pcache_page_limit(PCACHE_MAX_PAGES * PCACHE_TARGET_FRAME_DIVISOR),
            PCACHE_MAX_PAGES,
        );

        // 2 GiB of RAM expressed as 4 KiB frames reaches the 64 MiB cap.
        let two_gib_frames = (2usize * 1024 * 1024 * 1024) / 4096;
        assert_eq!(a20_pcache_page_limit(two_gib_frames), PCACHE_MAX_PAGES);
        assert_eq!(a20_pcache_page_limit(usize::MAX), PCACHE_MAX_PAGES);
    }
}
'''

        with tempfile.TemporaryDirectory(prefix="a20-pcache-capacity-") as directory:
            rust_source = Path(directory) / "capacity.rs"
            executable = Path(directory) / "capacity-tests"
            rust_source.write_text(constants + "\n" + function + "\n" + test_module)
            subprocess.run(
                [rustc, "--edition=2021", "--test", str(rust_source), "-o", str(executable)],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(executable)], check=False, text=True, capture_output=True
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
