#!/usr/bin/env python3
"""Compile the real PT MCS lock bodies with deterministic CPU migration hooks.

The functions are extracted verbatim from kernel/mm/pt.c so this test exercises
the production lock/unlock logic without compiling the rest of the kernel.
"""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PT_SOURCE = ROOT / "kernel/mm/pt.c"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start : end + 1]
    raise ValueError(f"unterminated function: {signature}")


PRELUDE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CONFIG_NR_CPUS 2
#define PT_MCS_POOL_SLOTS 8
#define A20_PERF_MM_PT_LOCK_ACQUIRES 1
#define A20_PERF_MM_PT_LOCK_CONTENDED 2
#define A20_PERF_MM_PT_LOCK_WAITS 3
typedef struct pt_mcs_node {
    volatile uintptr_t next;
    volatile uintptr_t locked;
} pt_mcs_node_t;
typedef struct pt_meta {
    uintptr_t lock;
    unsigned level;
} pt_meta_t;
typedef struct pt_mcs_pool {
    pt_mcs_node_t *nodes;
    pt_meta_t **held;
    uint32_t depth;
} pt_mcs_pool_t;
static pt_mcs_node_t nodes[CONFIG_NR_CPUS][PT_MCS_POOL_SLOTS];
static pt_meta_t *held[CONFIG_NR_CPUS][PT_MCS_POOL_SLOTS];
static pt_mcs_pool_t g_pt_mcs_pool[CONFIG_NR_CPUS];
static unsigned cpu_id;
static unsigned preempt_depth[CONFIG_NR_CPUS];
static int migrate_pending;

static unsigned cpu_current_id(void) {
    if (migrate_pending && preempt_depth[cpu_id] == 0) {
        migrate_pending = 0;
        cpu_id ^= 1;
    }
    return cpu_id;
}
static unsigned pt_cpu(void) {
    unsigned cpu = cpu_current_id();
    return cpu < CONFIG_NR_CPUS ? cpu : 0;
}
static void preempt_disable(void) { preempt_depth[cpu_id]++; }
static void preempt_enable(void) {
    assert(preempt_depth[cpu_id] != 0);
    preempt_depth[cpu_id]--;
}
static void a20_perf_count(unsigned event) { (void)event; }
static void arch_cpu_relax(void) { abort(); }
static void kerr(const char *fmt, ...) { (void)fmt; }
static void panic(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(99);
}
static void init(void) {
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        g_pt_mcs_pool[cpu].nodes = nodes[cpu];
        g_pt_mcs_pool[cpu].held = held[cpu];
        g_pt_mcs_pool[cpu].depth = 0;
        preempt_depth[cpu] = 0;
    }
    cpu_id = 0;
    migrate_pending = 0;
}
"""


HARNESS = r"""
int main(void) {
    init();
    pt_meta_t outer = {0}, inner = {0};

#ifdef TEST_NEGATIVE
    (void)inner;
    (void)preempt_disable;
    (void)mcs_lock(&outer);
    migrate_pending = 1;
    mcs_unlock(&outer);
    return 0;
#else
    migrate_pending = 1;
    uint32_t base = mcs_lock(&outer);
    assert(base == 0 && cpu_id == 0);
    migrate_pending = 1;
    uint32_t nested = mcs_lock(&inner);
    assert(nested == 1 && cpu_id == 0);
    assert(g_pt_mcs_pool[0].depth == 2);
    assert(preempt_depth[0] == 2);

    mcs_unlock(&inner);
    assert(g_pt_mcs_pool[0].depth == 1);
    assert(preempt_depth[0] == 1);
    assert(cpu_id == 0);
    mcs_unlock(&outer);
    assert(g_pt_mcs_pool[0].depth == 0);
    assert(preempt_depth[0] == 0);

    /* Migration is allowed as soon as the final MCS hold is released. */
    assert(cpu_current_id() == 1);
    assert(g_pt_mcs_pool[0].depth == 0);
    assert(g_pt_mcs_pool[1].depth == 0);
    return 0;
#endif
}
"""


def main() -> None:
    source = PT_SOURCE.read_text()
    lock = extract_function(source, "static uint32_t mcs_lock(pt_meta_t *m)")
    unlock = extract_function(source, "static void mcs_unlock(pt_meta_t *m)")
    with tempfile.TemporaryDirectory(prefix="a20-pt-mcs-test-") as tmp:
        tmp = Path(tmp)
        cc = ["cc", "-std=gnu11", "-O0", "-Wall", "-Wextra", "-Werror"]
        positive_src = tmp / "positive.c"
        positive_bin = tmp / "positive"
        positive_src.write_text(PRELUDE + lock + "\n" + unlock + "\n" + HARNESS)
        subprocess.run(cc + [str(positive_src), "-o", str(positive_bin)], check=True)
        subprocess.run([str(positive_bin)], check=True)

        # Negative control: the old code selected its per-CPU pool before the
        # simulated migration window had been closed; unlock must reject the
        # different CPU's empty/non-LIFO stack.
        negative_lock = lock.replace("    preempt_disable();", "    /* negative control: no pin */", 1)
        negative_src = tmp / "negative.c"
        negative_bin = tmp / "negative"
        negative_src.write_text(PRELUDE + negative_lock + "\n" + unlock + "\n" + HARNESS)
        subprocess.run(cc + ["-DTEST_NEGATIVE", str(negative_src), "-o",
                             str(negative_bin)], check=True)
        result = subprocess.run([str(negative_bin)], check=False)
        if result.returncode != 99:
            raise SystemExit(f"negative control unexpectedly returned {result.returncode}")
    print("test-pt-mcs-preempt-window: PASS (real source + migration negative control)")


if __name__ == "__main__":
    main()
