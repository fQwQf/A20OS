#!/usr/bin/env python3
"""Exercise production PFA range conversion, seeding, allocation, and freeing.

The generated host translation unit extracts the relevant functions from
ROOT/kernel/mm/frame.c and ROOT/kernel/include/mm/frame.h. Platform, lock, and
free-list plumbing are modeled locally. The embedded legacy allocator is a
negative control for physical-alignment handling.
"""
from pathlib import Path
import re
import subprocess
import tempfile

OLD_SEED_BLOCK = r'''
pfn_t start = (pfn_t)ROUND_UP((uint64_t)(range->start_pfn + used_frames), 1u << MAX_ORDER);

        for (pfn_t i = range->start_pfn + used_frames; i < start && i < range->end_pfn; i++) {
            meta[i].flags = FRAME_F_FREE;
            meta[i].refcount = 0;
            meta[i].order = 0;
            meta[i].prev = PFN_NONE;
            meta[i].next = PFN_NONE;
            fl_push(i, 0);
            pfa.free_frames++;
        }

        pfn_t remain = (range->end_pfn > start) ? range->end_pfn - start : 0;
        for (int o = MAX_ORDER; o >= 0 && remain > 0; o--) {
            pfn_t sz = 1u << o;
            while (remain >= sz) {
                meta[start].flags = FRAME_F_FREE;
                meta[start].refcount = 0;
                meta[start].order = (uint8_t)o;
                meta[start].prev = PFN_NONE;
                meta[start].next = PFN_NONE;
                fl_push(start, o);
                start  += sz;
                remain -= sz;
                pfa.free_frames += sz;
            }
        }
'''
OLD_ALLOCATOR = r'''

static pfn_t old_pfa_alloc_from_buddy(int order)
{
    int o;
    for (o = order; o <= MAX_ORDER; o++)
        if (pfa.free_lists[o].head != PFN_NONE)
            break;
    if (o > MAX_ORDER)
        return PFN_NONE;

    pfn_t blk = pfa.free_lists[o].head;
    fl_remove(blk, o);

    if (pfa.meta[blk].flags != FRAME_F_FREE || pfa.meta[blk].order != (uint8_t)o ||
        (blk & ((1u << o) - 1)) != 0) {
        printf("[PFA DOUBLE-ALLOC] pfn=%lu flags=0x%x refcount=%u order_meta=%d list_order=%d free_lists[%d].count=%u cpu=%u\n",
               (unsigned long)blk, pfa.meta[blk].flags, pfa.meta[blk].refcount,
               (int)pfa.meta[blk].order, o, o, pfa.free_lists[o].count, cpu_current_id());
        panic("pfa: double allocation detected");
    }

    while (o > order) {
        o--;
        pfn_t buddy = blk ^ (1u << o);
        /* The buddy half must be entirely free; if any interior frame is
         * still allocated the block was corrupt when it entered the list. */
        for (pfn_t i = buddy; i < buddy + (1u << o); i++) {
            if (pfa.meta[i].flags == FRAME_F_ALLOC ||
                pfa.meta[i].refcount > 0) {
                printf("[PFA DIRTY-SPLIT] blk=%lu order=%d buddy=%lu "
                       "sub=%lu flags=0x%x refcount=%u order_meta=%d cpu=%u\n",
                       (unsigned long)blk, o, (unsigned long)buddy,
                       (unsigned long)i, pfa.meta[i].flags,
                       pfa.meta[i].refcount, (int)pfa.meta[i].order,
                       cpu_current_id());
                panic("pfa: dirty split block");
            }
        }
        pfa.meta[buddy].flags = FRAME_F_FREE;
        pfa.meta[buddy].refcount = 0;
        pfa.meta[buddy].order = (uint8_t)o;
        pfa.meta[buddy].prev = PFN_NONE;
        pfa.meta[buddy].next = PFN_NONE;
        fl_push(buddy, o);
    }

    pfa.meta[blk].flags    = FRAME_F_ALLOC;
    pfa.meta[blk].refcount = 1;
    pfa.meta[blk].order    = (uint8_t)order;
    pfa.meta[blk].prev     = PFN_NONE;
    pfa.meta[blk].next     = PFN_NONE;

    for (pfn_t i = blk + 1; i < blk + (1u << order); i++) {
        if (pfa.meta[i].refcount > 0) continue;
        pfa.meta[i].flags    = FRAME_F_ALLOC;
        pfa.meta[i].refcount = 0;
        pfa.meta[i].order    = 0;
        pfa.meta[i].prev     = PFN_NONE;
        pfa.meta[i].next     = PFN_NONE;
    }

    pfa.free_frames -= (1u << order);
    return blk;
}
'''

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/mm/frame.c"
HEADER = ROOT / "kernel/include/mm/frame.h"


def extract_function(source: str, name: str) -> str:
    match = re.search(r"(?:static\s+)?(?:inline\s+)?[\w\s\*]+\b" +
                      re.escape(name) + r"\s*\([^;]*?\)\s*\{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.start()
    brace = source.find("{", match.start())
    depth = 0
    state = "code"
    i = brace
    while i < len(source):
        c = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if c == "/" and nxt == "*":
                state = "comment"
                i += 2
                continue
            if c == "/" and nxt == "/":
                state = "line"
                i += 2
                continue
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return source[start:i + 1]
        elif state == "comment":
            if c == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
        elif state == "line":
            if c == "\n":
                state = "code"
        elif state in ("string", "char"):
            if c == "\\":
                i += 2
                continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = "code"
        i += 1
    raise AssertionError(f"unterminated function: {name}")


HARNESS = r'''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096u
#define PAGE_SIZE_BITS 12
#define MAX_ORDER 11
#define PFN_NONE UINT32_MAX
#define PFA_MAX_RANGES 4
#define FRAME_F_FREE 0
#define FRAME_F_ALLOC 1
#define FRAME_F_KDATA 2
#define ROUND_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define PFA_FREEMAP_TEST(pfn) ((freemap[(pfn) >> 3] >> ((pfn) & 7)) & 1u)
#define PFA_FREEMAP_SET(pfn) (freemap[(pfn) >> 3] |= (uint8_t)(1u << ((pfn) & 7)))
#define PFA_FREEMAP_CLR(pfn) (freemap[(pfn) >> 3] &= (uint8_t)~(1u << ((pfn) & 7)))

typedef uint32_t pfn_t;
typedef uint64_t paddr_t;
typedef struct { paddr_t base, end; pfn_t start_pfn, end_pfn; } pfa_range_t;
typedef struct { pfn_t prev, next; uint16_t refcount; uint8_t order, flags; } frame_meta_t;
typedef struct { pfn_t head; size_t count; } free_list_t;
typedef struct {
    frame_meta_t *meta;
    pfn_t total_frames, free_frames;
    size_t nr_ranges;
    pfa_range_t ranges[PFA_MAX_RANGES];
    free_list_t free_lists[MAX_ORDER + 1];
    int lock;
} pfa_t;

#define MAX_TEST_FRAMES 40000
static frame_meta_t metadata[MAX_TEST_FRAMES];
static uint8_t freemap[(MAX_TEST_FRAMES + 7) / 8];
static uint8_t seen[MAX_TEST_FRAMES];
pfa_t pfa;

/* The actual conversion helpers from mm/frame.h are inserted here. */
static frame_meta_t *meta_of(pfn_t pfn) { return &pfa.meta[pfn]; }
static uint64_t spin_lock_irqsave(int *lock) { (void)lock; return 0; }
static void spin_unlock_irqrestore(int *lock, uint64_t flags) { (void)lock; (void)flags; }
static unsigned cpu_current_id(void) { return 0; }
static void frame_trace(pfn_t pfn) { (void)pfn; }
static void panic(const char *s) { fprintf(stderr, "panic: %s\n", s); abort(); }

/* Host list plumbing models the production free-list membership contract. */
static void fl_push(pfn_t pfn, int order) {
    frame_meta_t *m = meta_of(pfn);
    assert(!PFA_FREEMAP_TEST(pfn));
    m->prev = PFN_NONE;
    m->next = pfa.free_lists[order].head;
    if (m->next != PFN_NONE) meta_of(m->next)->prev = pfn;
    pfa.free_lists[order].head = pfn;
    pfa.free_lists[order].count++;
    PFA_FREEMAP_SET(pfn);
}
static void fl_remove(pfn_t pfn, int order) {
    frame_meta_t *m = meta_of(pfn);
    if (m->prev == PFN_NONE) pfa.free_lists[order].head = m->next;
    else meta_of(m->prev)->next = m->next;
    if (m->next != PFN_NONE) meta_of(m->next)->prev = m->prev;
    m->prev = m->next = PFN_NONE;
    assert(pfa.free_lists[order].count != 0);
    pfa.free_lists[order].count--;
    PFA_FREEMAP_CLR(pfn);
}

/* Production C functions are inserted here. */
'''


def main() -> int:
    src = SOURCE.read_text()
    header = HEADER.read_text()
    header_funcs = ["pfa_range_for_pfn", "pfa_range_for_pa", "pfn_to_phys",
                    "phys_to_pfn", "pfn_valid"]
    conversions = "\n".join(extract_function(header, name) for name in header_funcs)
    funcs = ["pfa_buddy_pfn", "fl_push_clean",
             "pfa_seed_range", "pfa_alloc_from_buddy", "pfa_free",
             "frame_put_locked", "pfa_split_allocated"]
    candidate_funcs = "\n".join(extract_function(src, name) for name in funcs)

    old_seed = (
        "\nstatic void old_seed_range(const pfa_range_t *range, pfn_t used_frames) {\n"
        "    frame_meta_t *meta = pfa.meta;\n" + OLD_SEED_BLOCK + "\n}\n"
    )
    old_alloc = OLD_ALLOCATOR
    generated = HARNESS.replace(
        "/* The actual conversion helpers from mm/frame.h are inserted here. */",
        conversions)
    generated += "\n" + candidate_funcs + old_seed + old_alloc
    generated += r'''
static void reset(void) {
    memset(&pfa, 0, sizeof(pfa));
    memset(metadata, 0, sizeof(metadata));
    memset(freemap, 0, sizeof(freemap));
    memset(seen, 0, sizeof(seen));
    pfa.meta = metadata;
    for (int o = 0; o <= MAX_ORDER; ++o) pfa.free_lists[o].head = PFN_NONE;
}
static void add_range(paddr_t base, size_t pages) {
    assert(pfa.nr_ranges < PFA_MAX_RANGES);
    pfa_range_t *r = &pfa.ranges[pfa.nr_ranges++];
    r->base = base;
    r->end = base + (paddr_t)pages * PAGE_SIZE;
    r->start_pfn = pfa.total_frames;
    r->end_pfn = pfa.total_frames + (pfn_t)pages;
    pfa.total_frames = r->end_pfn;
    assert(pfa.total_frames <= MAX_TEST_FRAMES);
}
static void seed_all(void) {
    pfa.free_frames = 0;
    for (size_t i = 0; i < pfa.nr_ranges; ++i)
        pfa_seed_range(&pfa.ranges[i], 0);
}
static void seed_reserved_range(const pfa_range_t *r, pfn_t used_frames) {
    for (pfn_t i = 0; i < used_frames; ++i) {
        frame_meta_t *m = &metadata[r->start_pfn + i];
        m->flags = FRAME_F_KDATA;
        m->refcount = 1;
        m->order = 0;
        m->prev = m->next = PFN_NONE;
    }
    pfa_seed_range(r, used_frames);
}
static void verify_lists(void) {
    memset(seen, 0, sizeof(seen));
    size_t pages = 0;
    for (int o = 0; o <= MAX_ORDER; ++o) {
        size_t walked = 0;
        for (pfn_t head = pfa.free_lists[o].head; head != PFN_NONE;
             head = metadata[head].next) {
            assert(head < pfa.total_frames);
            assert(metadata[head].order == (uint8_t)o);
            assert((pfn_to_phys(head) & (((paddr_t)PAGE_SIZE << o) - 1)) == 0);
            const pfa_range_t *r = pfa_range_for_pfn(head);
            assert(r != NULL);
            size_t count = (size_t)1u << o;
            assert(head + count <= r->end_pfn);
            for (pfn_t i = head; i < head + count; ++i) {
                assert(!seen[i]);
                seen[i] = 1;
                assert(PFA_FREEMAP_TEST(head));
                if (i != head) assert(!PFA_FREEMAP_TEST(i));
            }
            pages += count;
            walked++;
        }
        assert(walked == pfa.free_lists[o].count);
    }
    assert(pages == pfa.free_frames);
}
static void verify_all_free(void) {
    verify_lists();
    assert(pfa.free_frames == pfa.total_frames);
    for (pfn_t p = 0; p < pfa.total_frames; ++p) {
        assert(seen[p]);
        assert(metadata[p].refcount == 0);
    }
}
static void test_seed_layouts(void) {
    reset();
    add_range(0x00100000, 8192); /* physical base 1 MiB, compact PFN zero */
    seed_all();
    verify_lists();
    for (pfn_t p = 0; p < pfa.total_frames; ++p) assert(seen[p]);

    reset();
    add_range(0x00123000, 1500);
    add_range(0x04567000, 6000); /* nonzero compact start, unrelated PA phase */
    add_range(0x08001000, 17);   /* smaller than one maximum-order block */
    seed_all();
    verify_lists();
    for (pfn_t p = 0; p < pfa.total_frames; ++p) assert(seen[p]);

    /* Reserved metadata stays owned and is excluded from free accounting. */
    reset();
    add_range(0x00123000, 3500);
    const pfa_range_t *r = &pfa.ranges[0];
    pfn_t reserved = 19;
    seed_reserved_range(r, reserved);
    assert(pfa.free_frames == pfa.total_frames - reserved);
    verify_lists();
    for (pfn_t i = 0; i < reserved; ++i) {
        assert(!seen[i]);
        assert(metadata[i].flags == FRAME_F_KDATA);
        assert(metadata[i].refcount == 1);
    }

    /* A fully reserved range must not publish frames or alter its ownership. */
    reset();
    add_range(0x00901000, 29);
    r = &pfa.ranges[0];
    seed_reserved_range(r, r->end_pfn - r->start_pfn);
    assert(pfa.free_frames == 0);
    verify_lists();
}
static void test_multirange_orders(void) {
    for (int order = 0; order <= MAX_ORDER; ++order) {
        reset();
        add_range(0x00123000, 1500);
        add_range(0x04567000, 6000);
        add_range(0x08001000, 17);
        seed_all();
        pfn_t p = pfa_alloc_from_buddy(order);
        assert(p != PFN_NONE);
        paddr_t pa = pfn_to_phys(p);
        assert((pa & (((paddr_t)PAGE_SIZE << order) - 1)) == 0);
        const pfa_range_t *range = pfa_range_for_pfn(p);
        assert(range != NULL);
        assert(p + ((pfn_t)1u << order) <= range->end_pfn);
        pfa_free(p, order);
        verify_all_free();
    }
}
static void test_every_order_and_free_paths(void) {
    for (int order = 0; order <= MAX_ORDER; ++order) {
        reset();
        add_range(0x00100000, 16384);
        seed_all();
        pfn_t p = pfa_alloc_from_buddy(order);
        assert(p != PFN_NONE);
        paddr_t pa = pfn_to_phys(p);
        assert((pa & (((paddr_t)PAGE_SIZE << order) - 1)) == 0);
        assert(pfa.free_frames == pfa.total_frames - ((pfn_t)1u << order));
        pfa_free(p, order);
        verify_all_free();
    }

    /* Exercise both release implementations and reference-count ownership. */
    reset();
    add_range(0x00100000, 16384);
    seed_all();
    pfn_t p = pfa_alloc_from_buddy(0);
    assert(p != PFN_NONE);
    metadata[p].refcount = 2;
    frame_put_locked(p);
    assert(metadata[p].refcount == 1);
    frame_put_locked(p);
    verify_all_free();
}
static void test_split_wrap_and_random_release(void) {
    reset();
    add_range(0x00100000, 16384);
    seed_all();
    pfn_t pages[256];
    for (size_t i = 0; i < 256; ++i) {
        pages[i] = pfa_alloc_from_buddy(0);
        assert(pages[i] != PFN_NONE);
        assert((pfn_to_phys(pages[i]) & (PAGE_SIZE - 1)) == 0);
    }
    /* A fixed permutation makes merge order independent of the free-list order. */
    for (size_t i = 0; i < 256; ++i) {
        size_t idx = (i * 73u) & 255u;
        pfa_free(pages[idx], 0);
    }
    verify_lists();
    assert(pfa.free_frames == pfa.total_frames);
}
static int run_old_negative_control(void) {
    reset();
    add_range(0x00100000, 8192);
    old_seed_range(&pfa.ranges[0], 0);
    pfn_t p = old_pfa_alloc_from_buddy(MAX_ORDER);
    assert(p != PFN_NONE);
    paddr_t pa = pfn_to_phys(p);
    if ((pa & (((paddr_t)PAGE_SIZE << MAX_ORDER) - 1)) == 0)
        return 1;
    printf("OLD_NEGATIVE_CONTROL: old allocator returned misaligned PA 0x%llx\n",
           (unsigned long long)pa);
    return 0;
}
static void test_split_owned(void) {
    reset(); add_range(0x00100000, 16384); seed_all();
    pfn_t p = pfa_alloc_from_buddy(3);
    pfn_t free_before = pfa.free_frames;
    pfa.meta[p].refcount++;
    assert(pfa_split_allocated(p, 3) < 0 && pfa.meta[p].order == 3);
    pfa.meta[p].refcount--;
    assert(pfa_split_allocated(p, 2) < 0);
    assert(!pfa_split_allocated(p, 3));
    assert(pfa.free_frames == free_before);
    for (pfn_t i = 0; i < 8; i++) {
        assert(pfa.meta[p+i].order == 0 && pfa.meta[p+i].refcount == 1);
        /* Model independent DMA pin, VMO put and final DMA put. */
        pfa.meta[p+i].refcount++;
        frame_put_locked(p+i);
        assert(pfa.meta[p+i].refcount == 1);
        frame_put_locked(p+i);
    }
    verify_lists(); assert(pfa.free_frames == pfa.total_frames);
}
int main(void) {
    test_split_owned();
    test_seed_layouts();
    test_multirange_orders();
    test_every_order_and_free_paths();
    test_split_wrap_and_random_release();
    assert(run_old_negative_control() == 0);
    puts("pfa physical geometry candidate: PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="pfa-physical-host-") as td:
        cfile = Path(td) / "pfa_geometry.c"
        binary = Path(td) / "pfa_geometry"
        cfile.write_text(generated)
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-format",
                        str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    raise SystemExit(main())
