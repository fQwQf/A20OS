"""Host tests for the real VirtIO block driver's synchronous batch writer.

The production function body is brace-extracted from virtio_blk.c and linked
against a tiny deterministic model of request allocation/submission/waiting.
This exercises ownership and batching decisions without building a kernel or
pretending that a separately reimplemented algorithm is the code under test.
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/drivers/block/virtio_blk.c"


def extract_c_function(source: str, signature: str) -> str:
    """Extract one exact definition while ignoring braces in comments/strings."""
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
                    return source[start:i + 1]
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
    raise AssertionError(f"unterminated body for {signature!r}")


HOST_PRELUDE = r"""
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define VIRTIO_QUEUE_SIZE 64
#define VIRTIO_BLK_MAX_REQ_SLOTS (VIRTIO_QUEUE_SIZE / 3)
#define VIRTIO_BLK_REQ_SLOTS VIRTIO_BLK_MAX_REQ_SLOTS
#define VIRTIO_MAX_DEVS 4
#define VIRTIO_BLK_SECTOR_SIZE 512u
#define VIRTIO_BLK_MAX_TRANSFER_BYTES (1024u * 1024u)
#define VIRTIO_BLK_MAX_TRANSFER_SECTORS \
    (VIRTIO_BLK_MAX_TRANSFER_BYTES / VIRTIO_BLK_SECTOR_SIZE)

typedef struct block_dev {
    uint64_t capacity;
    void *priv;
} block_dev_t;
typedef struct block_write {
    uint64_t lba;
    const void *buf;
    size_t count;
} block_write_t;
typedef struct { int unused; } proc_wake_q_t;
typedef struct { int unused; } test_lock_t;
typedef struct virtio_blk_req {
    int in_use;
    int pending;
    const void *borrowed_buf;
    uint64_t lba;
} virtio_blk_req_t;
typedef struct virtio_blk_inst {
    int slot;
    int ever_published,closing;
    uint32_t active_calls;
    size_t request_slots;
    struct { int valid; } blk;
    test_lock_t lock;
    virtio_blk_req_t req[VIRTIO_BLK_REQ_SLOTS];
} virtio_blk_inst_t;

static virtio_blk_inst_t g_insts[VIRTIO_MAX_DEVS];
static unsigned alloc_calls, submit_calls, wait_calls, fallback_calls;
static unsigned in_use_now, max_outstanding, outstanding_at_first_wait;
static unsigned kick_calls, kicks_at_first_wait, submit_notify_nonzero;
static unsigned kick_submits[32];
static unsigned kick_waits[32];
static unsigned max_live = VIRTIO_BLK_REQ_SLOTS;
static unsigned full_on_alloc_call;
static unsigned fail_submit_call, fail_wait_call, close_on_submit;
static int fallback_result;
static int test_error;
static unsigned lock_depth, lock_calls, unlock_calls, wake_init_calls, wake_flush_calls;

static void reset(void) {
    memset(g_insts, 0, sizeof(g_insts));
    g_insts[0].slot = 0;
    g_insts[0].request_slots = VIRTIO_BLK_MAX_REQ_SLOTS;
    g_insts[0].blk.valid = 1;
    g_insts[0].ever_published = 1;
    alloc_calls = submit_calls = wait_calls = fallback_calls = 0;
    in_use_now = max_outstanding = outstanding_at_first_wait = 0;
    kick_calls = kicks_at_first_wait = submit_notify_nonzero = 0;
    memset(kick_submits, 0, sizeof(kick_submits));
    memset(kick_waits, 0, sizeof(kick_waits));
    max_live = VIRTIO_BLK_REQ_SLOTS;
    full_on_alloc_call = fail_submit_call = fail_wait_call = close_on_submit = 0;
    fallback_result = test_error = 0;
    lock_depth = lock_calls = unlock_calls = 0;
    wake_init_calls = wake_flush_calls = 0;
}
static void fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); test_error = 1; }
static void proc_wake_q_init(proc_wake_q_t *q) { (void)q; wake_init_calls++; }
static int proc_wake_q_flush(proc_wake_q_t *q) { (void)q; wake_flush_calls++; return 0; }
static uint64_t spin_lock_irqsave(test_lock_t *lock) {
    (void)lock;
    if (lock_depth) fail("recursive lock acquisition");
    lock_depth++;
    lock_calls++;
    return 0x1234;
}
static void spin_unlock_irqrestore(test_lock_t *lock, uint64_t flags) {
    (void)lock;
    if (flags != 0x1234 || lock_depth != 1) fail("lock/irq restore mismatch");
    lock_depth--;
    unlock_calls++;
}
static void virtio_blk_kick_locked(virtio_blk_inst_t *inst) {
    (void)inst;
    if (lock_depth != 1) fail("batch kick outside device lock");
    if (kick_calls < sizeof(kick_submits) / sizeof(kick_submits[0])) {
        kick_submits[kick_calls] = submit_calls;
        kick_waits[kick_calls] = wait_calls;
    } else {
        fail("too many batch kicks");
    }
    kick_calls++;
}
static virtio_blk_req_t *virtio_blk_alloc_req_locked(virtio_blk_inst_t *inst,
        proc_wake_q_t *wake_q, size_t sectors, int write, int flush) {
    (void)wake_q; (void)sectors; (void)write; (void)flush;
    alloc_calls++;
    if (lock_depth != 1) fail("allocation outside device lock");
    if (full_on_alloc_call == alloc_calls || in_use_now >= max_live) return NULL;
    for (unsigned i = 0; i < VIRTIO_BLK_REQ_SLOTS; i++) {
        if (!inst->req[i].in_use) {
            memset(&inst->req[i], 0, sizeof(inst->req[i]));
            inst->req[i].in_use = 1;
            return &inst->req[i];
        }
    }
    return NULL;
}
static int virtio_blk_submit_req(virtio_blk_inst_t *inst, virtio_blk_req_t *req,
        uint64_t lba, void *buf, size_t sectors, int write, int flush,
        int notify) {
    (void)inst; (void)sectors; (void)write; (void)flush;
    submit_calls++;
    if (!inst->active_calls) fail("submission without call ownership");
    if (close_on_submit == submit_calls) inst->closing = 1;
    if (notify) submit_notify_nonzero++;
    if (lock_depth != 1) fail("submission outside device lock");
    if (fail_submit_call == submit_calls) return -EIO;
    req->pending = 1;
    req->borrowed_buf = buf;
    req->lba = lba;
    in_use_now++;
    if (in_use_now > max_outstanding) max_outstanding = in_use_now;
    return 0;
}
static int virtio_blk_wait_req_timed(virtio_blk_inst_t *inst,
        virtio_blk_req_t *req, uint64_t lba) {
    (void)inst;
    if (lock_depth) fail("wait while device lock held");
    if (!req->pending || req->lba != lba) fail("wait did not own submitted request");
    if (!wait_calls) {
        outstanding_at_first_wait = in_use_now;
        kicks_at_first_wait = kick_calls;
    }
    wait_calls++;
    req->pending = 0;
    req->borrowed_buf = NULL;
    req->in_use = 0;
    if (!in_use_now) fail("outstanding underflow");
    in_use_now--;
    return fail_wait_call == wait_calls ? -EIO : 0;
}
static int blk_write_sector(block_dev_t *dev, uint64_t lba,
                            const void *buf, size_t count) {
    (void)dev; (void)lba; (void)buf; (void)count;
    if (lock_depth) fail("fallback called with lock held");
    fallback_calls++;
    return fallback_result;
}
"""


HOST_MAIN = r"""
static unsigned pending_count(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < VIRTIO_BLK_REQ_SLOTS; i++) {
        if (g_insts[0].req[i].pending || g_insts[0].req[i].borrowed_buf) count++;
    }
    return count;
}
static int check(int ok, const char *name) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", name); return 1; }
    return 0;
}
static void writes_init(block_write_t *writes, const void **buffers, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buffers[i] = (const void *)(uintptr_t)(0x10000 + i * 4096);
        writes[i] = (block_write_t){ .lba = 100 + i, .buf = buffers[i], .count = 1 };
    }
}
static int common_clean(void) {
    return lock_depth == 0 && in_use_now == 0 && pending_count() == 0 &&
           lock_calls == unlock_calls && wake_init_calls == wake_flush_calls;
}

int main(void) {
    int failed = 0;
    block_dev_t dev = { .capacity = UINT64_MAX, .priv = &g_insts[0] };

    /* Empty calls are a no-op, including NULL arguments. */
    reset();
    failed |= check(blk_write_batch(NULL, NULL, 0) == 0 && submit_calls == 0 &&
                    alloc_calls == 0, "zero-count no-op");

    /* More than sixteen vectors: one sixteen-request window, then a tail. */
    reset();
    block_write_t many[20]; const void *many_bufs[20];
    writes_init(many, many_bufs, 20);
    failed |= check(blk_write_batch(&dev, many, 20) == 0, "20-vector batch return");
    failed |= check(submit_calls == 20 && wait_calls == 20 && fallback_calls == 0,
                    "20-vector all submitted and waited");
    failed |= check(outstanding_at_first_wait == 16 && max_outstanding == 16,
                    "first wait after 16 outstanding");
    failed |= check(kick_calls == 2 && kicks_at_first_wait == 1 &&
                    kick_submits[0] == 16 && kick_submits[1] == 20 &&
                    kick_waits[0] == 0 && kick_waits[1] == 16,
                    "one kick per 16-vector window before waits");
    failed |= check(submit_notify_nonzero == 0,
                    "batch submit defers notify until window kick");
    failed |= check(common_clean(), "20-vector DMA ownership drained");

    /* The allocator fills after two requests; drain those before retrying the
       third slot, proving it does not spin or abandon a submitted prefix. */
    reset();
    block_write_t four[4]; const void *four_bufs[4];
    writes_init(four, four_bufs, 4);
    max_live = 2;
    failed |= check(blk_write_batch(&dev, four, 4) == 0, "temporary full return");
    failed |= check(submit_calls == 4 && wait_calls == 4 && fallback_calls == 0 &&
                    max_outstanding == 2, "temporary full drained and retried");
    failed |= check(kick_calls == 2 && kick_submits[0] == 2 &&
                    kick_submits[1] == 4 && kick_waits[0] == 0 &&
                    kick_waits[1] == 2,
                    "temporary-full windows each kicked once before draining");
    failed |= check(common_clean(), "temporary full ownership drained");

    /* No slots at all: the documented single-sector callback handles the
       current vector, and a later vector can be tried on the next iteration. */
    reset();
    block_write_t one[1]; const void *one_buf[1];
    writes_init(one, one_buf, 1);
    max_live = 0;
    failed |= check(blk_write_batch(&dev, one, 1) == 0 && fallback_calls == 1 &&
                    submit_calls == 0 && wait_calls == 0, "zero-slot fallback");
    failed |= check(common_clean(), "fallback ownership clean");

    /* A submission error after a prefix must still wait for every request in
       that prefix before returning the error. */
    reset();
    writes_init(four, four_bufs, 4);
    fail_submit_call = 3;
    failed |= check(blk_write_batch(&dev, four, 4) == -EIO, "submit error propagated");
    failed |= check(submit_calls == 3 && wait_calls == 2 && common_clean(),
                    "submit-error prefix drained");
    failed |= check(kick_calls == 1 && kick_submits[0] == 3 &&
                    kick_waits[0] == 0 && submit_notify_nonzero == 0,
                    "submit-error prefix kicked before draining");

    /* Wait errors are remembered while every later outstanding DMA is joined. */
    reset();
    writes_init(four, four_bufs, 4);
    fail_wait_call = 2;
    failed |= check(blk_write_batch(&dev, four, 4) == -EIO, "wait error propagated");
    failed |= check(submit_calls == 4 && wait_calls == 4 && common_clean(),
                    "wait error still drained all requests");
    failed |= check(kick_calls == 1 && kick_submits[0] == 4 && kick_waits[0] == 0,
                    "wait-error batch kicked once before waits");

    /* Validate all vectors before doing any IO: bad later entries and integer
       edge values cannot cause a valid prefix to be submitted first. */
    reset();
    writes_init(four, four_bufs, 4);
    four[3].lba = UINT64_MAX;
    failed |= check(blk_write_batch(&dev, four, 4) == -EINVAL && submit_calls == 0 &&
                    alloc_calls == 0 && common_clean(), "LBA overflow prevalidated");
    reset();
    writes_init(four, four_bufs, 4);
    four[3].count = SIZE_MAX;
    failed |= check(blk_write_batch(&dev, four, 4) == -EINVAL && submit_calls == 0 &&
                    alloc_calls == 0 && common_clean(), "count overflow prevalidated");

    reset();
    writes_init(four, four_bufs, 4);
    close_on_submit = 1;
    failed |= check(blk_write_batch(&dev, four, 4) == -ESHUTDOWN &&
                    submit_calls == 1 && wait_calls == 1 && kick_calls == 1 &&
                    !g_insts[0].active_calls && common_clean(),
                    "closing drains borrowed prefix and releases call ownership");
    failed |= check(blk_write_batch(&dev, four, 4) == -ENODEV && submit_calls == 1,
                    "closed admission rejects another call");

    if (failed) return 1;
    puts("virtio-blk-batch-host: extracted production function PASS");
    return 0;
}
"""


class VirtioBlkBatchHostTests(unittest.TestCase):
    def test_extracted_production_batch_writer(self):
        gcc = shutil.which("gcc")
        if not gcc:
            self.skipTest("gcc is required for this host oracle")
        source = SOURCE.read_text()
        function = "\n".join(extract_c_function(source, signature) for signature in (
            "static int virtio_blk_call_begin(", "static void virtio_blk_call_end(",
            "static int blk_write_batch_impl(", "static int blk_write_batch("))
        with tempfile.TemporaryDirectory(prefix="a20-virtio-blk-batch-") as tmp:
            c_file = pathlib.Path(tmp) / "host.c"
            exe = pathlib.Path(tmp) / "test-virtio-blk-batch"
            c_file.write_text(HOST_PRELUDE + "\n" + function + "\n" + HOST_MAIN)
            subprocess.run(
                [gcc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                 str(c_file), "-o", str(exe)],
                check=True, cwd=ROOT,
            )
            result = subprocess.run([str(exe)], check=False, text=True,
                                    capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("extracted production function PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
