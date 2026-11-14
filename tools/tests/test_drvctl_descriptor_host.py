"""Host regression for drvctl's ELF driver-descriptor validation.

The C parser is extracted from user/cmds/core/drvctl.c.  Fixture fields come
from the kernel descriptor header, and the test first checks that drvctl's
mirrored constants remain synchronized with that header.
"""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
DRVCTL = ROOT / "user/cmds/core/drvctl.c"
KERNEL_HEADER = ROOT / "kernel/include/drivers/driver_descriptor.h"


def macro_value(source: str, name: str) -> int:
    match = re.search(rf"^\s*#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)U?\b",
                      source, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing numeric macro {name}")
    return int(match.group(1), 0)


def extract_braced_definition(source: str, signature: str) -> str:
    """Return a C typedef/function through its matching brace and terminator."""
    if source.count(signature) != 1:
        raise AssertionError(f"expected exactly one occurrence of {signature!r}")
    start = source.index(signature)
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing body for {signature!r}")
    depth = 0
    i = brace
    state = "code"
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if c == "/" and n == "*":
                state = "block_comment"
                i += 1
            elif c == "/" and n == "/":
                state = "line_comment"
                i += 1
            elif c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    end = i + 1
                    while end < len(source) and source[end].isspace():
                        end += 1
                    if end < len(source) and source[end] == ";":
                        end += 1
                    elif source.startswith("typedef", start):
                        # A typedef's alias follows the closing brace (for
                        # example `} a20_driver_desc_t;`), so include it too.
                        semicolon = source.find(";", end)
                        if semicolon < 0:
                            raise AssertionError(
                                f"missing typedef terminator for {signature!r}")
                        end = semicolon + 1
                    return source[start:end]
        elif state == "block_comment":
            if c == "*" and n == "/":
                state = "code"
                i += 1
        elif state == "line_comment":
            if c == "\n":
                state = "code"
        elif state == "string":
            if c == "\\":
                i += 1
            elif c == '"':
                state = "code"
        elif state == "char":
            if c == "\\":
                i += 1
            elif c == "'":
                state = "code"
        i += 1
    raise AssertionError(f"unterminated definition {signature!r}")


HARNESS_MAIN = r"""
int main(int argc, char **argv) {
    if (argc != 4) return 10;
    a20_driver_desc_t desc;
    if (read_descriptor(argv[1], &desc) != 0) {
        fprintf(stderr, "valid kernel ABI descriptor was rejected\n");
        return 1;
    }
    if (read_descriptor(argv[2], &desc) == 0) {
        fprintf(stderr, "previous ABI descriptor was accepted\n");
        return 2;
    }
    if (read_descriptor(argv[3], &desc) == 0) {
        fprintf(stderr, "wrong descriptor version was accepted\n");
        return 3;
    }
    puts("drvctl-descriptor-host: extracted production parser PASS");
    return 0;
}
"""


class DrvctlDescriptorHostTests(unittest.TestCase):
    def test_kernel_descriptor_guard_rejects_stale_and_future_abi(self):
        cc=shutil.which("cc")
        if not cc:self.skipTest("cc is required")
        with tempfile.TemporaryDirectory(prefix="a20-kernel-driver-abi-") as tmp:
            source=pathlib.Path(tmp)/"test.c";exe=pathlib.Path(tmp)/"test"
            source.write_text(r"""
#include <assert.h>
#include "drivers/driver_descriptor.h"
int main(void) {
    a20_driver_descriptor_t descriptor = {
        .magic=A20_DRIVER_DESCRIPTOR_MAGIC, .version=A20_DRIVER_DESCRIPTOR_VERSION,
        .placement=A20_DRIVER_PLACEMENT_KERNEL_MODULE,
        .type=A20_DRIVER_TYPE_DISPLAY, .name="gpu-host", .abi=A20_DRIVER_ABI
    };
    assert(a20_driver_descriptor_sane(&descriptor));
    descriptor.abi=A20_DRIVER_ABI-1;
    assert(!a20_driver_descriptor_sane(&descriptor));
    descriptor.abi=A20_DRIVER_ABI+1;
    assert(!a20_driver_descriptor_sane(&descriptor));
    return 0;
}
""")
            subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-I"+str(ROOT/"kernel/include"),str(source),"-o",str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_real_parser_accepts_only_kernel_abi_descriptor(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("cc is required for this x86 host test")
        drvctl_text = DRVCTL.read_text()
        kernel_text = KERNEL_HEADER.read_text()
        names = ("A20_DRIVER_DESCRIPTOR_MAGIC", "A20_DRIVER_DESCRIPTOR_VERSION",
                 "A20_DRIVER_ABI", "A20_DRIVER_MAX_MATCH")
        kernel_values = {name: macro_value(kernel_text, name) for name in names}
        drvctl_values = {name: macro_value(drvctl_text, name) for name in names}
        self.assertEqual(drvctl_values, kernel_values,
                         "drvctl descriptor constants differ from the kernel header")

        typedefs = "\n".join(
            extract_braced_definition(drvctl_text, signature)
            for signature in ("typedef struct a20_driver_match_desc {",
                              "typedef struct a20_driver_desc {")
        )
        parser = extract_braced_definition(drvctl_text,
                                           "static int read_descriptor(")
        self.assertEqual(kernel_values["A20_DRIVER_MAX_MATCH"], 4)
        host_prelude = f"""
#define _POSIX_C_SOURCE 200809L
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define A20_DRIVER_DESCRIPTOR_MAGIC {drvctl_values['A20_DRIVER_DESCRIPTOR_MAGIC']}U
#define A20_DRIVER_DESCRIPTOR_VERSION {drvctl_values['A20_DRIVER_DESCRIPTOR_VERSION']}U
#define A20_DRIVER_ABI {drvctl_values['A20_DRIVER_ABI']}U
#define A20_DRIVER_MAX_MATCH {drvctl_values['A20_DRIVER_MAX_MATCH']}U
#define NAME_MAX_LEN 48
{typedefs}
_Static_assert(sizeof(a20_driver_desc_t) == 112, "descriptor layout drift");
"""

        def fixture_source(abi: int, version: int) -> str:
            return f"""
#include <stdint.h>
#define A20_DRIVER_MAX_MATCH {kernel_values['A20_DRIVER_MAX_MATCH']}U
{typedefs}
#define FIXTURE_MAGIC {kernel_values['A20_DRIVER_DESCRIPTOR_MAGIC']}U
__attribute__((used, section(".a20drv")))
static const a20_driver_desc_t fixture = {{
    .magic = FIXTURE_MAGIC,
    .version = {version}U,
    .placement = 1U,
    .type = 1U,
    .name = "host-fixture",
    .abi = {abi}U,
    .match_count = 1U,
    .match = {{ {{ 1U, 0x1234U, 0x5678U }} }},
}};
"""

        with tempfile.TemporaryDirectory(prefix="a20-drvctl-desc-") as tmp:
            tmp_path = pathlib.Path(tmp)
            harness = tmp_path / "harness.c"
            exe = tmp_path / "test-drvctl-descriptor"
            harness.write_text(host_prelude + "\n" + parser + "\n" + HARNESS_MAIN)
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(harness), "-o", str(exe)], check=True, cwd=ROOT)

            fixture_paths = []
            fixture_specs = (
                ("current", kernel_values["A20_DRIVER_ABI"],
                 kernel_values["A20_DRIVER_DESCRIPTOR_VERSION"]),
                ("old-abi", kernel_values["A20_DRIVER_ABI"] - 1,
                 kernel_values["A20_DRIVER_DESCRIPTOR_VERSION"]),
                ("wrong-version", kernel_values["A20_DRIVER_ABI"],
                 kernel_values["A20_DRIVER_DESCRIPTOR_VERSION"] + 1),
            )
            for name, abi, version in fixture_specs:
                c_file = tmp_path / f"{name}.c"
                obj_file = tmp_path / f"{name}.o"
                c_file.write_text(fixture_source(abi, version))
                subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                "-c", str(c_file), "-o", str(obj_file)],
                               check=True, cwd=ROOT)
                fixture_paths.append(obj_file)
            result = subprocess.run([str(exe), *(str(p) for p in fixture_paths)],
                                    check=False, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("extracted production parser PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
