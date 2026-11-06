#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MM_PT_META_ENTRIES 512
#define MM_ST_INVALID 0u
#define MM_ST_PT_NODE 1u
#define MM_ST_CLASS_MAX 16u
#define MM_ST_CLS_BYTE(c) ((c)<<4)
#define MM_ST_COW_BIT 8u
#define MM_ST_PROT_MASK 7u
#define PTE_COW 256u
static uint8_t mm_pt_prot_bits(uintptr_t pte) { return pte&7; }
#define MM_ST_ANON_MAPPED 2u
#define MM_ST_GET_CLASS(b) (((b) >> 4) & 0xFu)
#define MM_SAFE_MASK 0x0fu
#define PAGE_SIZE 4096UL

typedef uint32_t pfn_t;
typedef uintptr_t pte_t;
typedef uintptr_t vaddr_t;
typedef pte_t pt_root_t;
typedef struct page_cache_page { uintptr_t token; } page_cache_page_t;
typedef struct pt_meta {
    uintptr_t lock, node;
    uint16_t nr_present;
    uint8_t level, stale, fork_file_clean;
    uint8_t cls[MM_PT_META_ENTRIES];
    uint8_t safe[(MM_PT_META_ENTRIES + 7) / 8];
    void *segtab;
    page_cache_page_t **cache_owner;
    void *share;
} pt_meta_t;

static pte_t table[MM_PT_META_ENTRIES];
static pt_meta_t meta;
static page_cache_page_t page = { .token = 0x1234 };
static unsigned locks, unlocks;

static pt_meta_t *mm_pt_meta(pte_t *t) { return t == table ? &meta : NULL; }
static pte_t *mm_pt_leaf_table(pt_root_t *root, vaddr_t va) {
    (void)va;
    return root == table ? table : NULL;
}
static int arch_pt_vpn(vaddr_t va, int level) {
    (void)level;
    return (int)((va >> 12) & 511);
}
static void mm_pt_node_lock(pte_t *t) { (void)t; meta.fork_file_clean=0; locks++; }
static void mm_pt_node_unlock(pte_t *t) { (void)t; unlocks++; }
static uint8_t owner_page[4096];
static pfn_t pfa_alloc_flags(int order, int can_reclaim) {
    assert(order == 0 && can_reclaim == 0);
    return 0x1000;
}
static void *pfn_to_virt(uint32_t pfn) {
    assert(pfn == 0x1000);
    return owner_page;
}
static uint32_t virt_to_pfn(const void *ptr) {
    assert(ptr == owner_page);
    return 0x1000;
}
static void pfa_free(uint32_t pfn, int order) {
    assert(pfn == 0x1000 && order == 0);
}
#define PFN_NONE 0

/* REAL_CACHE_HELPERS */

int main(void) {
    const vaddr_t va = 0x2345000;
    const int idx = arch_pt_vpn(va, 0);
    mm_pt_cache_note(table, va, NULL);
    assert(meta.cache_owner == NULL && mm_pt_cache_peek(table, idx) == NULL);

    mm_pt_cache_note(table, va, &page);
    assert(meta.cache_owner != NULL && mm_pt_cache_peek(table, idx) == &page);
    assert(locks == 1 && unlocks == 1);

    mm_pt_note_present(table, 0, idx, MM_ST_ANON_MAPPED << 4);
    assert(meta.nr_present == 1 && mm_pt_cache_peek(table, idx) == NULL);

    mm_pt_cache_note(table, va, &page);
    assert(mm_pt_cache_peek(table, idx) == &page);
    mm_pt_note_absent(table, 0, idx);
    assert(meta.nr_present == 0 && mm_pt_cache_peek(table, idx) == NULL);
    assert(meta.safe[idx >> 3] == 0 && locks == 2 && unlocks == 2);

    pfa_free(virt_to_pfn(meta.cache_owner), 0);
    meta.cache_owner = NULL;
    mm_pt_note_present(table, 0, 0, MM_ST_CLS_BYTE(MM_ST_ANON_MAPPED));
    meta.fork_file_clean = 1;
    assert(mm_pt_safe_set(table, 0, 0, 1) == 0 && !meta.fork_file_clean);
    meta.fork_file_clean = 1;
    assert(mm_pt_safe_clear(table, 0, 0, 1) == 0 && !meta.fork_file_clean);
    meta.fork_file_clean = 1;
    assert(mm_pt_refresh_leaf_prot(table, 0, 3) == 0 && !meta.fork_file_clean);
    meta.fork_file_clean = 1;
    assert(mm_pt_sync_status_meta(&meta, 0, MM_ST_ANON_MAPPED, 3) == 0);
    assert(!meta.fork_file_clean);
    meta.fork_file_clean = 1;
    mm_pt_note_present(table, 0, 0, MM_ST_CLS_BYTE(MM_ST_ANON_MAPPED));
    assert(!meta.fork_file_clean);
    meta.fork_file_clean = 1;
    mm_pt_note_absent(table, 0, 0);
    assert(!meta.fork_file_clean);
    meta.fork_file_clean = 1;
    mm_pt_meta_set_stale(&meta, 1);
    assert(!meta.fork_file_clean && meta.stale);
    return 0;
}
