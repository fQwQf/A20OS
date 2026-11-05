#!/usr/bin/env python3
"""Compile the production ABI feature queries in Native and BOTH profiles."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent.parent
source = (ROOT / "kernel/abi/native/sys_core.c").read_text()

def function(name, declaration="int64_t "):
    start = source.index(declaration + name + "(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "user/liba20rt/a20_types.h"
#include "kernel/include/uapi/a20/abi_features.h"
#define A20_ABI_MAJOR 1
#define A20_ABI_MINOR 1
#define A20_ABI_PATCH 0
#define A20_ARG(n) args->arg[n]
typedef struct { uint64_t arg[6]; } a20_syscall_args_t;
static int copy_to_user(void *to, const void *from, size_t size) {
    memcpy(to, from, size);
    return 0;
}
'''
harness += function("native_feature_word0", "static uint64_t ") + "\n"
harness += function("sys_a20_abi_info") + "\n" + function("sys_a20_feature_test")
harness += r'''
int main(void) {
    a20_abi_info_t info;
    a20_syscall_args_t args = {{0}};
    assert(sys_a20_abi_info(&args) == -A20_ERR_FAULT);
    args.arg[0] = (uintptr_t)&info;
    memset(&info, 0xa5, sizeof(info));
    assert(sys_a20_abi_info(&args) == 0);
    assert(info.size == sizeof(info) && info.version == 1);
    assert(info.abi_major == 1 && info.abi_minor == 1 && info.abi_patch == 0);
    for (unsigned i = 0; i < 300; i++) {
        args.arg[0] = i;
        /* Published 1.1 feature set: bits 0..15, with negotiated Linux exec
         * and 64-bit-only tags for exec/cleanup/notification exit. */
        int supported = i < 16;
        if (i == 4) supported = EXPECT_LINUX;
        if ((i == 7 || i == 9 || i == 13) && sizeof(uintptr_t) != 8)
            supported = 0;
        assert(sys_a20_feature_test(&args) == (supported ? 0 : -A20_ERR_NOT_SUPPORTED));
        if (i < 256)
            assert(!!(info.feature_bits[i / 64] & (1ULL << (i % 64))) == supported);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="a20-exec-features-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    for profile, expected in [("NATIVE", 0), ("BOTH", 1)]:
        binary = path / profile
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT), "-DCONFIG_ABI_" + profile,
                        "-DEXPECT_LINUX=" + str(expected), str(path / "test.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print("PASS production ABI queries:", profile)
