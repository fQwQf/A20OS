/* Host harness for the production pt_walk/pt_map_cls owner reuse. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 4096UL
#define ARCH_PT_ROOT_LEVEL 2
#define PTE_V 0x001UL
#define PTE_DIR 0x002UL
#define PTE_LEAF 0x004UL
#define PTE_X 0x008UL
#define PTE_COW 0x010UL
#define PTE_W 0x020UL
#define PTE_U 0x040UL
#define PTE_R 0x080UL
#define MM_ST_ANON_MAPPED 2
#define MM_ST_FILE_PRIVATE 4
#define MM_ST_FILE_SHARED 5
#define MM_ST_VMO 6
#define MM_ST_PT_NODE 8
#define MM_ST_CLS_BYTE(cls) ((uint8_t)(((cls) & 0x0f) << 4))
#define MM_ST_COW_BIT 0x08
#define MM_FAIL_STAGE_PT_NODE 1
#define ENOMEM 12
#define EEXIST 17
#ifdef CONFIG_SWAP
#define SWAP_ENTRY 0x200UL
#endif

typedef uintptr_t pte_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;
typedef uint32_t pfn_t;
typedef pte_t pt_root_t;

static _Alignas(PAGE_SIZE) pte_t root[512];
static _Alignas(PAGE_SIZE) pte_t allocated_tables[4][512];
static unsigned allocation_calls, allocation_fail_on, initialized_nodes;
static unsigned fail_inject, fail_inject_calls;
static unsigned frame_put_calls, flush_calls, valid_pfn_calls;
static unsigned note_calls, leaf_table_calls;
static pfn_t last_frame_put;
static pte_t *last_note_owner;
static int last_note_level, last_note_index;
static uint8_t last_note_class;
static uintptr_t last_flush_addr;
static size_t last_flush_size;
static int pfn_is_valid = 1;

static int arch_pt_vpn(uintptr_t va, int level) {
    return (int)((va >> (12 + 9 * level)) & 511);
}
static pte_t *arch_pte_to_ptr(pte_t pte) {
    return (pte_t *)(uintptr_t)(pte & ~(PAGE_SIZE - 1));
}
static pte_t arch_pte_from_pa(paddr_t pa) { return (pte_t)pa | PTE_V; }
static int arch_pte_is_leaf(pte_t pte) { return (pte & PTE_LEAF) != 0; }
static paddr_t arch_pte_addr(pte_t pte) { return pte & ~(PAGE_SIZE - 1); }
static pte_t arch_pte_leaf(paddr_t pa, pte_t flags) {
    return pa | flags | PTE_V | PTE_LEAF;
}
static paddr_t va_to_pa(const void *ptr) { return (uintptr_t)ptr; }
static pte_t *frame_alloc(void) {
    allocation_calls++;
    if (allocation_fail_on && allocation_calls == allocation_fail_on)
        return NULL;
    assert(allocation_calls <= 4);
    return allocated_tables[allocation_calls - 1];
}
static int mm_fail_inject_scoped(int stage) {
    (void)stage;
    fail_inject_calls++;
    return fail_inject;
}
static void mm_pt_node_init(pte_t *table, int level) {
    int found = 0;
    for (unsigned i = 0; i < 4; ++i)
        found |= table == allocated_tables[i];
    assert(found);
    (void)level;
    initialized_nodes++;
}
static void mm_pt_note_present(pte_t *table, int level, int index, uint8_t cls) {
    note_calls++;
    last_note_owner = table;
    last_note_level = level;
    last_note_index = index;
    last_note_class = cls;
}
static pfn_t phys_to_pfn(paddr_t pa) { return (pfn_t)(pa / PAGE_SIZE); }
static void frame_put(pfn_t pfn) {
    frame_put_calls++;
    last_frame_put = pfn;
}
static int pfn_valid(pfn_t pfn) {
    (void)pfn;
    valid_pfn_calls++;
    return pfn_is_valid;
}
static void *pfn_to_virt(pfn_t pfn) {
    return (void *)(uintptr_t)(0x100000UL + (uintptr_t)pfn * PAGE_SIZE);
}
static void arch_flush_icache_range(void *address, size_t size) {
    flush_calls++;
    last_flush_addr = (uintptr_t)address;
    last_flush_size = size;
}
static uint8_t mm_pt_prot_bits(pte_t flags) {
    return (uint8_t)(((flags & PTE_R) ? 0x01 : 0) |
                     ((flags & PTE_W) ? 0x02 : 0) |
                     ((flags & PTE_X) ? 0x04 : 0));
}
pte_t *mm_pt_leaf_table(pte_t *pgdir, uintptr_t va) {
    (void)pgdir;
    (void)va;
    leaf_table_calls++;
    return NULL;
}
#ifdef CONFIG_SWAP
static int pte_is_swap(pte_t pte) { return pte == SWAP_ENTRY; }
#endif

/* WALK_HELPER */
/* PT_WALK_WRAPPER */
/* PT_INSTALL_LEAF */
/* PT_MAP_CLS */

static const uintptr_t test_va = 0x12345000UL;
static void reset(void) {
    memset(root, 0, sizeof(root));
    memset(allocated_tables, 0, sizeof(allocated_tables));
    allocation_calls = allocation_fail_on = initialized_nodes = 0;
    fail_inject = fail_inject_calls = frame_put_calls = flush_calls = 0;
    valid_pfn_calls = note_calls = leaf_table_calls = 0;
    last_frame_put = 0;
    last_note_owner = NULL;
    last_note_level = last_note_index = -1;
    last_note_class = 0;
    last_flush_addr = 0;
    last_flush_size = 0;
    pfn_is_valid = 1;
}
static void install_existing_tables(void) {
    pte_t *middle = allocated_tables[0];
    pte_t *leaf = allocated_tables[1];
    root[arch_pt_vpn(test_va, 2)] = arch_pte_from_pa(va_to_pa(middle)) | PTE_V | PTE_DIR;
    middle[arch_pt_vpn(test_va, 1)] = arch_pte_from_pa(va_to_pa(leaf)) | PTE_V | PTE_DIR;
}

int main(void) {
    /* A read-only walk of a missing subtree allocates nothing and clears the
     * caller's stale owner output. */
    reset();
    pte_t *owner = (pte_t *)(uintptr_t)1;
    assert(pt_walk_with_owner(root, test_va, 0, &owner) == NULL);
    assert(owner == NULL && allocation_calls == 0 && fail_inject_calls == 0);

    /* Both scoped injection and allocator failure leave owner NULL. The
     * second allocator failure preserves a valid partial intermediate chain. */
    reset();
    fail_inject = 1;
    owner = (pte_t *)(uintptr_t)1;
    assert(pt_walk_with_owner(root, test_va, 1, &owner) == NULL);
    assert(owner == NULL && allocation_calls == 0 && initialized_nodes == 0);
    reset();
    fail_inject = 1;
    assert(pt_map_cls(root, test_va, 19 * PAGE_SIZE, PTE_R,
                      MM_ST_FILE_PRIVATE) == -ENOMEM);
    assert(allocation_calls == 0 && note_calls == 0 && frame_put_calls == 0);
    reset();
    allocation_fail_on = 1;
    assert(pt_walk_with_owner(root, test_va, 1, &owner) == NULL);
    assert(owner == NULL && allocation_calls == 1 && initialized_nodes == 0);
    reset();
    allocation_fail_on = 2;
    assert(pt_walk_with_owner(root, test_va, 1, &owner) == NULL);
    assert(owner == NULL && allocation_calls == 2 && initialized_nodes == 1);
    assert(root[arch_pt_vpn(test_va, 2)] & PTE_V);
    reset();
    allocation_fail_on = 2;
    assert(pt_map_cls(root, test_va, 19 * PAGE_SIZE, PTE_R,
                      MM_ST_FILE_PRIVATE) == -ENOMEM);
    assert(allocation_calls == 2 && initialized_nodes == 1 && note_calls == 1);
    assert(root[arch_pt_vpn(test_va, 2)] & PTE_V);

    /* A huge leaf conflicts with a level-0 walk; no owner is published. */
    reset();
    root[arch_pt_vpn(test_va, 2)] = 0x800000UL | PTE_V | PTE_LEAF;
    owner = (pte_t *)(uintptr_t)1;
    assert(pt_walk_with_owner(root, test_va, 1, &owner) == NULL);
    assert(owner == NULL && allocation_calls == 0);
    assert(pt_map_cls(root, test_va, 19 * PAGE_SIZE, PTE_R,
                      MM_ST_FILE_SHARED) == -ENOMEM);
    assert(owner == NULL && allocation_calls == 0);

    /* Existing hierarchy returns its exact leaf slot and owning table. */
    reset();
    install_existing_tables();
    assert(pt_walk_with_owner(root, test_va, 0, &owner) ==
           &allocated_tables[1][arch_pt_vpn(test_va, 0)]);
    assert(owner == allocated_tables[1]);
    assert(pt_walk(root, test_va, 0) ==
           &allocated_tables[1][arch_pt_vpn(test_va, 0)]);

    /* A new leaf mapping allocates two tables, records source class plus
     * COW/protection status on the same owner, and does not root-walk again. */
    reset();
    const uint8_t source_class = 7;
    pte_t flags = PTE_X | PTE_COW | PTE_U | PTE_R;
    assert(pt_map_cls(root, test_va, 19 * PAGE_SIZE, flags, source_class) == 0);
    pte_t *leaf_slot = &allocated_tables[1][arch_pt_vpn(test_va, 0)];
    assert(*leaf_slot == arch_pte_leaf(19 * PAGE_SIZE, flags));
    assert(last_note_owner == allocated_tables[1] && last_note_level == 0);
    assert(last_note_index == arch_pt_vpn(test_va, 0));
    assert(last_note_class == (MM_ST_CLS_BYTE(source_class) | MM_ST_COW_BIT |
                               mm_pt_prot_bits(flags)));
    assert(note_calls == 3 && leaf_table_calls == 0);
    assert(frame_put_calls == 0 && flush_calls == 1 &&
           last_flush_addr == (uintptr_t)pfn_to_virt(19) &&
           last_flush_size == PAGE_SIZE);

    /* Replacement drops only the displaced leaf reference; same-PA update
     * keeps it. Both operations use the existing owner without root-walking. */
    reset();
    install_existing_tables();
    pte_t *slot = &allocated_tables[1][arch_pt_vpn(test_va, 0)];
    *slot = arch_pte_leaf(8 * PAGE_SIZE, PTE_R);
    assert(pt_map_cls(root, test_va, 9 * PAGE_SIZE, PTE_R,
                      MM_ST_ANON_MAPPED) == 0);
    assert(frame_put_calls == 1 && last_frame_put == 8 && leaf_table_calls == 0);
    reset();
    install_existing_tables();
    slot = &allocated_tables[1][arch_pt_vpn(test_va, 0)];
    *slot = arch_pte_leaf(8 * PAGE_SIZE, PTE_R);
    assert(pt_map_cls(root, test_va, 8 * PAGE_SIZE, PTE_R,
                      MM_ST_ANON_MAPPED) == 0);
    assert(frame_put_calls == 0 && leaf_table_calls == 0);

    reset();
    pfn_is_valid = 0;
    assert(pt_map_cls(root, test_va, 19 * PAGE_SIZE, PTE_X,
                      MM_ST_FILE_PRIVATE) == 0);
    assert(valid_pfn_calls == 1 && flush_calls == 0);

#ifdef CONFIG_SWAP
    /* Match the legacy walk's swap interruption contract: the returned PTE
     * and owner are in the current table, without attempting allocation. */
    reset();
    int root_index = arch_pt_vpn(test_va, ARCH_PT_ROOT_LEVEL);
    root[root_index] = SWAP_ENTRY;
    assert(pt_walk_with_owner(root, test_va, 1, &owner) == &root[root_index]);
    assert(owner == root && allocation_calls == 0);
#endif
    puts("PT_MAP_OWNER: real helper and pt_map_cls PASS owner reuse, statuses, refs, exec sync and failures");
    return 0;
}
