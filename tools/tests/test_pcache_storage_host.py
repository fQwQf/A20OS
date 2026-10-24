"""Host fixture for the production page-cache fill/ownership transaction."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected one {signature!r}")

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


PRELUDE = r'''
#![no_std]

use core::ffi::{c_int, c_uint};
use core::sync::atomic::Ordering;

pub const PCACHE_HASH_BUCKETS: usize = __PCACHE_HASH_BUCKETS__;
pub const EVICT_SKIP: i32 = 0;
pub const EVICT_TAKE: i32 = 1;
pub const EVICT_SECOND_CHANCE: i32 = 2;

#[repr(C)]
pub struct PcacheEntry {
    pub page_no: u64,
    pub dirty: c_int,
    pub dirty_gen: u64,
    pub ref_: c_int,
    pub valid: c_int,
    pub accessed: u8,
    pub held: c_int,
    pub data: *mut u8,
    pub prev: *mut PcacheEntry,
    pub next: *mut PcacheEntry,
    pub hnext: *mut PcacheEntry,
}

#[repr(C)]
pub struct PcacheDirtySet {
    pub count: usize,
    pub pool: *mut PcacheEntry,
    pub pool_count: usize,
    pub words: *mut u64,
}

pub type BcacheBucketLockFn = extern "C" fn(*mut core::ffi::c_void, u64) -> u64;
pub type BcacheBucketUnlockFn = extern "C" fn(*mut core::ffi::c_void, u64, u64);
pub type BcacheLockFn = extern "C" fn(*mut core::ffi::c_void) -> u64;
pub type BcacheUnlockFn = extern "C" fn(*mut core::ffi::c_void, u64);
pub type BcachePageActionFn = extern "C" fn(*mut core::ffi::c_void, u64);
pub type BcachePageFlushFn =
    extern "C" fn(*mut core::ffi::c_void, *mut PcacheEntry) -> c_int;
pub type BcachePageReadFn =
    extern "C" fn(*mut core::ffi::c_void, u64, *mut u8) -> c_int;
pub type PcachePageAllocFn = extern "C" fn(*mut core::ffi::c_void) -> *mut u8;
pub type PcachePageFreeFn = extern "C" fn(*mut u8);
pub type BcachePagePerfFn = extern "C" fn(*mut core::ffi::c_void, c_int);
pub type BcachePageDiagnosticFn = extern "C" fn(*mut core::ffi::c_void, u64);
'''

C_SOURCE = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define N 2000
#define HASH 512
#define MAX UINT64_MAX

void rust_eh_personality(void) {}

typedef struct PcacheEntry {
    uint64_t page_no;
    int dirty;
    uint64_t dirty_gen;
    int ref_;
    int valid;
    uint8_t accessed;
    int held;
    uint8_t *data;
    struct PcacheEntry *prev;
    struct PcacheEntry *next;
    struct PcacheEntry *hnext;
} PcacheEntry;

typedef struct {
    size_t count;
    PcacheEntry *pool;
    size_t pool_count;
    uint64_t *words;
} PcacheDirtySet;

typedef struct {
    PcacheEntry pool[N];
    PcacheEntry dup;
    PcacheEntry head;
    PcacheEntry tail;
    PcacheEntry *hash[HASH];
    int fail_alloc;
    int fail_read;
    int make_dup;
    int allocs;
    int reads;
    int flushes;
    int frees;
    int global_locked;
    int fill_locked;
    int wb_locked;
    PcacheDirtySet dirty_pages;
    uint64_t dirty_words[(N + 63) / 64];
} Ctx;

extern unsigned a20_pcache_hash_key(uint64_t);
extern void a20_bcache_ref_get(int *);
extern void a20_bcache_ref_put(int *);
extern void a20_bcache_set_page_dirty(PcacheDirtySet *, PcacheEntry *, int);
extern void a20_pcache_lru_insert_front(PcacheEntry *, PcacheEntry *);
extern void a20_pcache_lru_remove(PcacheEntry *);
extern void a20_pcache_hash_insert_locked(PcacheEntry **, PcacheEntry *);
extern void a20_pcache_release_storage(
    PcacheEntry *, size_t, void (*)(uint8_t *));
extern PcacheEntry *a20_pcache_get_transaction(
    void *, PcacheEntry *, size_t, PcacheEntry **, PcacheEntry *,
    PcacheEntry *, PcacheDirtySet *, uint64_t, const uint8_t *, int,
    uint64_t (*)(void *, uint64_t),
    void (*)(void *, uint64_t, uint64_t),
    uint64_t (*)(void *),
    void (*)(void *, uint64_t),
    void (*)(void *, uint64_t),
    void (*)(void *, uint64_t),
    void (*)(void *, uint64_t),
    void (*)(void *, uint64_t),
    int (*)(void *, PcacheEntry *),
    int (*)(void *, uint64_t, uint8_t *),
    uint8_t *(*)(void *),
    void (*)(void *, int),
    void (*)(void *, uint64_t),
    void (*)(void *, uint64_t));

/* Each callback checks the lock boundary expected by the production txn. */
static uint64_t lock_bucket(void *p, uint64_t n)
{
    (void)p;
    (void)n;
    return 1;
}

static void unlock_bucket(void *p, uint64_t n, uint64_t flags)
{
    (void)p;
    (void)n;
    assert(flags == 1);
}

static uint64_t lock_global(void *p)
{
    Ctx *ctx = p;
    assert(!ctx->global_locked);
    ctx->global_locked = 1;
    return 2;
}

static void unlock_global(void *p, uint64_t flags)
{
    Ctx *ctx = p;
    assert(ctx->global_locked && flags == 2);
    ctx->global_locked = 0;
}

static void fill_lock(void *p, uint64_t n)
{
    (void)n;
    Ctx *ctx = p;
    assert(!ctx->fill_locked);
    ctx->fill_locked = 1;
}

static void fill_unlock(void *p, uint64_t n)
{
    (void)n;
    Ctx *ctx = p;
    assert(ctx->fill_locked);
    ctx->fill_locked = 0;
}

static void writeback_lock(void *p, uint64_t n)
{
    (void)n;
    Ctx *ctx = p;
    assert(!ctx->wb_locked);
    ctx->wb_locked = 1;
}

static void writeback_unlock(void *p, uint64_t n)
{
    (void)n;
    Ctx *ctx = p;
    assert(ctx->wb_locked);
    ctx->wb_locked = 0;
}

static int flush_victim(void *p, PcacheEntry *entry)
{
    Ctx *ctx = p;
    (void)entry;
    assert(!ctx->global_locked && ctx->wb_locked);
    ctx->flushes++;
    return 0;
}

static int read_page(void *p, uint64_t page_no, uint8_t *data)
{
    Ctx *ctx = p;
    assert(!ctx->global_locked && !ctx->wb_locked && ctx->fill_locked);
    ctx->reads++;

    if (ctx->make_dup) {
        ctx->make_dup = 0;
        ctx->dup.page_no = page_no;
        ctx->dup.valid = 1;
        ctx->dup.ref_ = 0;
        ctx->dup.prev = &ctx->head;
        ctx->dup.next = ctx->head.next;
        ctx->head.next->prev = &ctx->dup;
        ctx->head.next = &ctx->dup;
        ctx->hash[a20_pcache_hash_key(page_no)] = &ctx->dup;
    }

    if (ctx->fail_read)
        return -5;
    memset(data, (int)page_no, 4096);
    return 0;
}

static uint8_t *alloc_page(void *p)
{
    Ctx *ctx = p;
    assert(!ctx->global_locked && !ctx->wb_locked && ctx->fill_locked);
    ctx->allocs++;
    if (ctx->fail_alloc)
        return NULL;
    return calloc(1, 4096);
}

static void perf(void *p, int event)
{
    (void)p;
    (void)event;
}

static void diagnostic(void *p, uint64_t page_no)
{
    (void)p;
    (void)page_no;
}

static void note_error(void *p, uint64_t page_no)
{
    (void)p;
    (void)page_no;
}

/* Slot ownership lasts through failed fills, duplicates, and eviction. */
static void free_page(uint8_t *data)
{
    if (data) {
        extern Ctx g;
        g.frees++;
        free(data);
    }
}

Ctx g;

static void init(void)
{
    memset(&g, 0, sizeof(g));
    g.dirty_pages.pool = g.pool;
    g.dirty_pages.pool_count = N;
    g.dirty_pages.words = g.dirty_words;
    g.head.next = &g.tail;
    g.tail.prev = &g.head;
    for (size_t i = 0; i < N; i++) {
        g.pool[i].page_no = MAX;
        a20_pcache_lru_insert_front(&g.head, &g.pool[i]);
        if (i != 0)
            g.pool[i].ref_ = 1;
    }
}

static PcacheEntry *get(uint64_t page_no)
{
    return a20_pcache_get_transaction(
        &g, g.pool, N, g.hash, &g.head, &g.tail, &g.dirty_pages,
        page_no, NULL, 0, lock_bucket, unlock_bucket,
        lock_global, unlock_global, fill_lock, fill_unlock,
        writeback_lock, writeback_unlock, flush_victim, read_page,
        alloc_page, perf, diagnostic, note_error);
}

static void test_alloc_failure(void)
{
    init();
    g.fail_alloc = 1;
    assert(get(7) == NULL);
    assert(g.allocs == 1 && g.reads == 0 && !g.fill_locked &&
           !g.global_locked && !g.wb_locked);
    assert(g.pool[0].ref_ == 0 && !g.pool[0].valid &&
           g.pool[0].page_no == MAX && g.pool[0].data == NULL);
    assert(g.dirty_pages.count == 0 && g.dirty_words[0] == 0);
}

static void test_dirty_set_count_generation(void)
{
    init();
    PcacheEntry *entry = &g.pool[0];
    a20_bcache_set_page_dirty(&g.dirty_pages, entry, 1);
    assert(g.dirty_pages.count == 1 && g.dirty_words[0] == 1 &&
           entry->dirty_gen == 1);
    a20_bcache_set_page_dirty(&g.dirty_pages, entry, 1);
    assert(g.dirty_pages.count == 1 && g.dirty_words[0] == 1 &&
           entry->dirty_gen == 2);
    a20_bcache_set_page_dirty(&g.dirty_pages, entry, 0);
    assert(g.dirty_pages.count == 0 && g.dirty_words[0] == 0 &&
           entry->dirty_gen == 2);
    a20_bcache_set_page_dirty(&g.dirty_pages, entry, 1);
    assert(g.dirty_pages.count == 1 && g.dirty_words[0] == 1 &&
           entry->dirty_gen == 3);
}

static void test_dirty_set_word_edges(void)
{
    init();
    PcacheEntry *at_63 = &g.pool[63];
    PcacheEntry *at_64 = &g.pool[64];
    PcacheEntry *last = &g.pool[1999];
    a20_bcache_set_page_dirty(&g.dirty_pages, at_63, 1);
    a20_bcache_set_page_dirty(&g.dirty_pages, at_64, 1);
    a20_bcache_set_page_dirty(&g.dirty_pages, last, 1);
    assert(g.dirty_pages.count == 3 && g.dirty_words[0] == (1ull << 63) &&
           g.dirty_words[1] == 1 && g.dirty_words[31] == (1ull << 15));
    a20_bcache_set_page_dirty(&g.dirty_pages, at_64, 0);
    assert(g.dirty_pages.count == 2 && g.dirty_words[1] == 0 &&
           g.dirty_words[0] == (1ull << 63) &&
           g.dirty_words[31] == (1ull << 15));
}

static void test_read_retry_and_release(void)
{
    init();
    g.fail_read = 1;
    assert(get(8) == NULL);
    uint8_t *saved = g.pool[0].data;
    assert(saved && g.allocs == 1 && g.pool[0].page_no == MAX &&
           !g.pool[0].valid);
    assert(g.dirty_pages.count == 0 && g.dirty_words[0] == 0);

    g.fail_read = 0;
    PcacheEntry *entry = get(8);
    assert(entry == &g.pool[0] && entry->data == saved &&
           g.allocs == 1 && g.reads == 2);
    assert(g.dirty_pages.count == 0 && g.dirty_words[0] == 0);
    a20_bcache_ref_put(&entry->ref_);
    assert(g.dirty_pages.count == 0 && g.dirty_words[0] == 0);

    a20_pcache_release_storage(g.pool, N, free_page);
    assert(g.frees == 1 && g.pool[0].data == NULL);
    a20_pcache_release_storage(g.pool, N, free_page);
    assert(g.frees == 1);
}

static void test_duplicate_retains_payload(void)
{
    init();
    g.make_dup = 1;
    PcacheEntry *entry = get(9);
    assert(entry == &g.dup && g.allocs == 1 && g.pool[0].data &&
           g.pool[0].ref_ == 0 && !g.pool[0].valid);
    a20_bcache_ref_put(&entry->ref_);

    a20_pcache_release_storage(g.pool, N, free_page);
    assert(g.frees == 1 && g.pool[0].data == NULL);
}

static void test_eviction_reuses_slot_payload(void)
{
    init();
    PcacheEntry *entry = get(10);
    assert(entry == &g.pool[0]);
    uint8_t *saved = entry->data;
    a20_bcache_ref_put(&entry->ref_);
    a20_bcache_set_page_dirty(&g.dirty_pages, entry, 1);
    entry->valid = 1;
    assert(g.dirty_pages.count == 1 && g.dirty_words[0] == 1);

    PcacheEntry *next = get(11);
    assert(next == entry && next->data == saved && g.allocs == 1 &&
           g.flushes == 1 && g.reads == 2);
    assert(g.dirty_pages.count == 0 && g.dirty_words[0] == 0);
    a20_bcache_ref_put(&next->ref_);

    a20_pcache_release_storage(g.pool, N, free_page);
    assert(g.frees == 1);
}

int main(void)
{
    test_alloc_failure();
    test_dirty_set_count_generation();
    test_dirty_set_word_edges();
    test_read_retry_and_release();
    test_duplicate_retains_payload();
    test_eviction_reuses_slot_payload();
    puts("pcache-storage: production Rust transaction ownership tests PASS");
    return 0;
}
'''


class PcacheStorageTests(unittest.TestCase):
    def test_production_fill_and_release(self):
        rustc, cc = shutil.which("rustc"), shutil.which("cc")
        if not rustc or not cc:
            self.skipTest("rustc and cc are required for this host fixture")

        source = (ROOT / "kernel/fs/block_cache_state.rs").read_text()
        header = (ROOT / "kernel/include/fs/block_cache.h").read_text()
        rust_hash = int(re.search(
            r"pub const PCACHE_HASH_BUCKETS: usize = (\d+);", source
        ).group(1))
        c_hash = int(re.search(r"#define PCACHE_HASH_BUCKETS\s+(\d+)", header).group(1))
        self.assertEqual(rust_hash, c_hash)
        signatures = [
            'fn pcache_dirty_slot(',
            'pub extern "C" fn a20_pcache_hash_key(',
            'pub unsafe extern "C" fn a20_bcache_ref_read(',
            'pub unsafe extern "C" fn a20_bcache_ref_get(',
            'pub unsafe extern "C" fn a20_bcache_ref_put(',
            'pub unsafe extern "C" fn a20_pcache_lru_insert_front(',
            'pub unsafe extern "C" fn a20_pcache_lru_remove(',
            'pub unsafe extern "C" fn a20_pcache_hash_insert_locked(',
            'pub unsafe extern "C" fn a20_pcache_hash_remove_locked(',
            'pub unsafe extern "C" fn a20_pcache_find_locked(',
            'pub unsafe extern "C" fn a20_bcache_set_page_dirty(',
            'pub extern "C" fn a20_bcache_evict_decide(',
            'pub unsafe extern "C" fn a20_pcache_evict_transaction(',
            'pub unsafe extern "C" fn a20_pcache_get_transaction(',
            'pub unsafe extern "C" fn a20_pcache_release_storage(',
        ]
        functions = "\n".join(
            "#[no_mangle]\n" + extract_function(source, signature)
            for signature in signatures
        )

        with tempfile.TemporaryDirectory(prefix="a20-pcache-storage-") as directory:
            tmp = Path(directory)
            rust_source = tmp / "storage.rs"
            rust_library = tmp / "libstorage.a"
            c_source = tmp / "host.c"
            executable = tmp / "host"

            rust_source.write_text(
                PRELUDE.replace("__PCACHE_HASH_BUCKETS__", str(rust_hash))
                + functions
                + "\n#[panic_handler] fn panic(_: &core::panic::PanicInfo<'_>) -> ! { loop {} }\n"
            )
            c_source.write_text(C_SOURCE)
            subprocess.run(
                [
                    rustc,
                    "--edition=2021",
                    "--crate-type",
                    "staticlib",
                    "-C",
                    "panic=abort",
                    "-C",
                    "opt-level=2",
                    str(rust_source),
                    "-o",
                    str(rust_library),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run(
                [
                    cc,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-O2",
                    str(c_source),
                    str(rust_library),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(executable)], check=False, text=True, capture_output=True
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("production Rust transaction", result.stdout)


if __name__ == "__main__":
    unittest.main()
