#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t pte_t;
typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;
typedef uint32_t pfn_t;
typedef pte_t pt_root_t;
typedef struct { int level; void **cache_owner; } pt_meta_t;
typedef struct {
    paddr_t base, end;
    pfn_t start_pfn, end_pfn;
} pfa_range_t;
typedef struct { size_t nr_ranges; pfa_range_t ranges[4]; } pfa_t;
static pfa_t pfa;

#define PAGE_SIZE 4096UL
#define PAGE_SIZE_BITS 12
#define PAGE_OFFSET 0x100000000UL
#define PFN_NONE ((uint32_t)-1)
#define USER_VA_LIMIT 0x0000800000000000UL
#define ARCH_PT_ROOT_LEVEL 3
#define ARCH_HAS_PGTABLE_OPS 1
#define PTE_V (1UL << 0)
#define PTE_W (1UL << 1)
#define PTE_U (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_COW (1UL << 4)
#define EINVAL 22
#define EEXIST 17
#define ENOMEM 12
#define MM_ST_ANON_MAPPED 2u
#define MM_ST_ANON_SHARED 3u
#define MM_ST_FILE_PRIVATE 4u
#define MM_ST_FILE_SHARED 5u
#define A20_COW_ANON_BATCH_PAGES 16
#define MM_ST_CLS_BYTE(c) ((uint8_t)(c))
#define MM_ST_COW_BIT 0x80u

struct a20_cow_anon_batch_page {
    uint32_t pfn;
    uint32_t class;
    uintptr_t flags;
    void *pcp;
    uint32_t kind;
};
/* ANON_BATCH_ABI_ASSERTS */
static pte_t child_slots[512];
static void *cache_slots[512];
static pt_meta_t child_meta = { .level = 0 };
static size_t walk_calls, installs, status_calls, exec_flushes, frame_puts;
static int walk_fail, upper_conflict, bad_meta;
/* REAL_PFA_CONVERSIONS */

static pte_t *pt_walk_with_owner(pt_root_t *root, vaddr_t va, int alloc,
                                 pte_t **owner_out) {
    (void)root; (void)alloc; walk_calls++;
    if (owner_out) *owner_out = NULL;
    if (walk_fail || upper_conflict) return NULL;
    if (owner_out) *owner_out = child_slots;
    return &child_slots[(va >> 12) & 511];
}
static pt_meta_t *mm_pt_meta(pte_t *table) { (void)table; return &child_meta; }
static int mm_pt_meta_level(const pt_meta_t *meta) { return bad_meta ? 1 : meta->level; }
static pfn_t pfa_alloc_flags(int order, int can_reclaim) {
    (void)order; (void)can_reclaim;
    return PFN_NONE;
}
static int arch_pt_vpn(vaddr_t va, int level) { return (int)((va >> (12 + 9 * level)) & 511); }
static int arch_pt_level_entries(int level) { (void)level; return 512; }
static int arch_pte_is_leaf(pte_t pte) { return (pte & PTE_V) != 0; }
static paddr_t arch_pte_addr(pte_t pte) { return pte & ~0xfffUL; }
#define PFN2PA(pfn) ((paddr_t)(pfn) * PAGE_SIZE)
static void frame_put(pfn_t pfn) { assert(pfn != 0); frame_puts++; }
static void arch_flush_icache_range(void *ptr, size_t len) { (void)ptr; assert(len == PAGE_SIZE); exec_flushes++; }
static pte_t arch_pte_leaf(paddr_t pa, pte_t flags) { installs++; return pa | flags | PTE_V; }
static void mm_pt_note_present(pte_t *owner, int level, int idx, uint8_t cls) {
    assert(owner == child_slots); assert(level == 0); assert(idx >= 0 && idx < 512);
    assert((cls & 0x7f) == MM_ST_ANON_MAPPED ||
           (cls & 0x7f) == MM_ST_ANON_SHARED ||
           (cls & 0x7f) == MM_ST_FILE_PRIVATE ||
           (cls & 0x7f) == MM_ST_FILE_SHARED); status_calls++;
}
static void mm_pt_note_present_meta(pt_meta_t *meta, int idx, uint8_t cls) {
    (void)meta;
    mm_pt_note_present(child_slots, 0, idx, cls);
}
static int mm_pt_prot_bits(pte_t flags) { (void)flags; return 0; }

/* REAL_LEAF_INSTALL */
/* REAL_SCALAR_MAPPER */
/* REAL_BATCH_MAPPER */

static void reset(void) {
    memset(&pfa, 0, sizeof(pfa));
    pfa.nr_ranges=2;
    pfa.ranges[0]=(pfa_range_t){.base=0x00100000,.end=0x00120000,
                               .start_pfn=1000,.end_pfn=1032};
    pfa.ranges[1]=(pfa_range_t){.base=0x04000000,.end=0x04020000,
                               .start_pfn=5000,.end_pfn=5032};
    memset(child_slots, 0, sizeof(child_slots));
    memset(cache_slots, 0, sizeof(cache_slots));
    child_meta.cache_owner = cache_slots;
    walk_calls=installs=status_calls=exec_flushes=frame_puts=0;
    walk_fail=upper_conflict=bad_meta=0; child_meta.level=0;
}
static void fill(struct a20_cow_anon_batch_page *pages, size_t n, uintptr_t flags) {
    for (size_t i=0;i<n;i++) pages[i]=(struct a20_cow_anon_batch_page){
        .pfn=(uint32_t)(i<32 ? 1000+i : 5000+i-32),
        .class=MM_ST_ANON_MAPPED, .flags=flags, .pcp=NULL, .kind=1 };
}
int main(void) {
    struct a20_cow_anon_batch_page pages[A20_COW_ANON_BATCH_PAGES];
    const vaddr_t start=0x20000;
    reset(); fill(pages,16,PTE_U|PTE_X);
    reset();
    assert(pt_map_cls(child_slots,start,0x00100000,PTE_U|PTE_X,MM_ST_ANON_MAPPED)==0);
    assert(walk_calls==1 && installs==1 && status_calls==1 && exec_flushes==1);
    assert(child_slots[32] == (0x00100000|PTE_U|PTE_X|PTE_V));
    reset(); child_slots[32]=0x00101000|PTE_V|PTE_U;
    assert(pt_map_cls(child_slots,start,0x00100000,PTE_U,MM_ST_ANON_MAPPED)==0);
    assert(walk_calls==1 && installs==1 && status_calls==1 && frame_puts==1);

    reset();
    assert(pt_map_anon_batch(child_slots,start,pages,16,0)==0);
    assert(walk_calls==1 && installs==16 && status_calls==16 && exec_flushes==16);
    assert(frame_puts==0);
    for(size_t i=0;i<16;i++) {
        paddr_t expected_pa = i < 32
            ? 0x00100000 + i * PAGE_SIZE
            : 0x04000000 + (i - 32) * PAGE_SIZE;
        if (child_slots[32+i] != (expected_pa|PTE_U|PTE_X|PTE_V)) {
            fprintf(stderr, "EXPECTED_PA_MISMATCH index=%zu got=%#llx expected=%#llx\n",
                    i, (unsigned long long)child_slots[32+i],
                    (unsigned long long)(expected_pa|PTE_U|PTE_X|PTE_V));
            return 77;
        }
        assert(child_slots[32+i] == (expected_pa|PTE_U|PTE_X|PTE_V));
    }

    reset(); fill(pages,16,PTE_U|PTE_X);
    pages[0].class=MM_ST_FILE_PRIVATE;
    pages[0].kind=2;
    pages[0].pcp=&pages[0];
    pages[7].class=MM_ST_FILE_PRIVATE;
    pages[7].kind=2;
    pages[7].pcp=&pages[7];
    pages[15].class=MM_ST_FILE_PRIVATE;
    assert(pt_map_anon_batch(child_slots,start,pages,16,0)==0);
    assert(walk_calls==1 && installs==16 && status_calls==16 && exec_flushes==16);
    assert(cache_slots[32] == &pages[0] && cache_slots[39] == &pages[7]);
    assert(cache_slots[47] == NULL);

    reset(); fill(pages,16,PTE_U|PTE_X);
    pages[0].class=MM_ST_ANON_SHARED;
    pages[0].flags|=PTE_W;
    pages[15].class=MM_ST_FILE_SHARED;
    pages[15].flags|=PTE_W;
    assert(pt_map_anon_batch(child_slots,start,pages,16,1)==0);
    assert(walk_calls==1 && installs==16 && status_calls==16 && exec_flushes==16);

    reset(); fill(pages,16,PTE_U|PTE_X);
    pages[5]=(struct a20_cow_anon_batch_page){0};
    pages[12]=(struct a20_cow_anon_batch_page){0};
    assert(pt_map_anon_batch(child_slots,start,pages,16,0)==0);
    assert(walk_calls==1 && installs==14 && status_calls==14 && exec_flushes==14);
    assert(child_slots[32+5]==0 && child_slots[32+12]==0);

    for (size_t bad_at_i=0; bad_at_i<4; bad_at_i++) {
        reset(); fill(pages,16,PTE_U|PTE_X);
        if (bad_at_i==0) pages[7].pfn=0;
        if (bad_at_i==1) pages[7].class=6;
        if (bad_at_i==2) pages[7].flags=PTE_W|PTE_U;
        if (bad_at_i==3) pages[7].flags=PTE_X;
        assert(pt_map_anon_batch(child_slots,start,pages,16,0)<0);
        assert(walk_calls==0 && installs==0 && status_calls==0 && exec_flushes==0 && frame_puts==0);
    }

    reset(); fill(pages,16,PTE_U);
    assert(pt_map_anon_batch(child_slots,start,pages,0,0)<0);
    assert(pt_map_anon_batch(child_slots,start,pages,A20_COW_ANON_BATCH_PAGES+1,0)<0);
    assert(pt_map_anon_batch(child_slots,start+1,pages,16,0)<0);
    assert(pt_map_anon_batch(child_slots,USER_VA_LIMIT-PAGE_SIZE,pages,16,0)<0);
    assert(pt_map_anon_batch(child_slots,0x1ff000,pages,16,0)<0); /* crosses L0 */
    assert(walk_calls==0 && installs==0);

    for (int conflict=0; conflict<3; conflict++) {
        reset(); fill(pages,16,PTE_U);
        child_slots[32+(conflict==0?0:conflict==1?15:7)] =
            conflict==2 ? (0x50000 | 0x200) : (PTE_V|PTE_U);
        assert(pt_map_anon_batch(child_slots,start,pages,16,0)==-17);
        assert(walk_calls==1 && installs==0 && status_calls==0 && frame_puts==0);
    }

    reset(); fill(pages,16,PTE_U); upper_conflict=1; /* huge leaf or swap above L0 */
    assert(pt_map_anon_batch(child_slots,start,pages,16,0)==-ENOMEM);
    assert(walk_calls==1 && installs==0 && status_calls==0 && frame_puts==0);

    reset(); fill(pages,16,PTE_U); walk_fail=1;
    assert(pt_map_anon_batch(child_slots,start,pages,16,0)<0);
    assert(walk_calls==1 && installs==0 && status_calls==0 && frame_puts==0);
    reset(); fill(pages,16,PTE_U); bad_meta=1;
    assert(pt_map_anon_batch(child_slots,start,pages,16,0)<0);
    assert(walk_calls==1 && installs==0 && status_calls==0 && frame_puts==0);
    reset(); fill(pages,A20_COW_ANON_BATCH_PAGES,PTE_U);
    assert(pt_map_anon_batch(child_slots,start,pages,A20_COW_ANON_BATCH_PAGES,0)==0);
    assert(installs==A20_COW_ANON_BATCH_PAGES && status_calls==A20_COW_ANON_BATCH_PAGES);
    puts("ANON_CHILD_BATCH_C_MAPPER: actual extracted functions, whole-run preflight and install PASS");
    return 0;
}
