#include "hyp/hyp_s2.h"
#include "mm/frame.h"
#include <assert.h>
#include <string.h>
extern int puts(const char *);

pfa_t pfa;
static _Alignas(4096) uint8_t pages[TEST_FRAMES][PAGE_SIZE];
static pt_meta_t metadata[TEST_FRAMES];
static int table_count, mapped_count, live_frames, alloc_budget = -1;
static int metadata_fail, invalidation_result;
static unsigned invalidations;
static uint64_t last_invalidated;
static paddr_t physical_base = 0x12300000;

int pfn_valid(pfn_t p) { return p < TEST_FRAMES; }
void *pfn_to_virt(pfn_t p) { assert(pfn_valid(p)); return pages[p]; }
paddr_t pfn_to_phys(pfn_t p) { assert(pfn_valid(p)); return physical_base + (uint64_t)p * PAGE_SIZE; }
pfn_t phys_to_pfn(paddr_t pa) {
    if (pa < physical_base || pa - physical_base >= sizeof(pages)) return PFN_NONE;
    return (pa - physical_base) / PAGE_SIZE;
}
pfn_t virt_to_pfn(const void *p) {
    uintptr_t delta = (uintptr_t)p - (uintptr_t)pages;
    assert(delta < sizeof(pages) && !(delta & (PAGE_SIZE - 1)));
    return delta / PAGE_SIZE;
}
pfn_t pfa_alloc_flags(int order, int reclaim) {
    assert(order == 0 && reclaim == 0);
    if (!alloc_budget) return PFN_NONE;
    if (alloc_budget > 0) --alloc_budget;
    for (pfn_t p = 0; p < TEST_FRAMES; ++p) {
        if (!pfa.meta[p].refcount) {
            pfa.meta[p].refcount = 1; pfa.meta[p].flags = FRAME_F_ALLOC;
            memset(pages[p], 0xcc, PAGE_SIZE); ++live_frames; return p;
        }
    }
    return PFN_NONE;
}
void pfa_free(pfn_t p, int order) {
    assert(!order && pfn_valid(p) && pfa.meta[p].refcount == 1);
    assert(!(pfa.meta[p].flags & (FRAME_F_PT | FRAME_F_GUEST)));
    memset(&pfa.meta[p], 0, sizeof(pfa.meta[p])); --live_frames;
}
void frame_get(pfn_t p) { assert(pfn_valid(p) && pfa.meta[p].refcount); ++pfa.meta[p].refcount; }
void frame_put(pfn_t p) {
    assert(pfn_valid(p) && pfa.meta[p].refcount);
    if (pfa.meta[p].refcount == 1) pfa_free(p, 0);
    else --pfa.meta[p].refcount;
}
int mm_pt_node_init_atomic(pte_t *p, unsigned level) {
    if (metadata_fail) return -ENOMEM;
    pfn_t n = virt_to_pfn(p);
    assert(pfa.meta[n].refcount == 1 && pfa.meta[n].flags == FRAME_F_ALLOC);
    pfa.meta[n].flags |= FRAME_F_PT;
    memset(&metadata[n], 0, sizeof(metadata[n])); metadata[n].level = level;
    return 0;
}
void mm_pt_node_fini(pte_t *p) {
    pfn_t n = virt_to_pfn(p); assert(pfa.meta[n].flags & FRAME_F_PT);
    pfa.meta[n].flags &= ~FRAME_F_PT;
}
pt_meta_t *mm_pt_meta(pte_t *p) {
    pfn_t n = virt_to_pfn(p); return pfa.meta[n].flags & FRAME_F_PT ? &metadata[n] : NULL;
}
void mm_pt_note_present(pte_t *p, unsigned level, unsigned i, uint8_t c) {
    pt_meta_t *m = mm_pt_meta(p); assert(m && level == m->level && i < 512); m->cls[i] = c;
}
void mm_pt_note_absent(pte_t *p, unsigned level, unsigned i) {
    pt_meta_t *m = mm_pt_meta(p); assert(m && level == m->level && i < 512); m->cls[i] = 0;
}
void mm_pt_frame_return(pfn_t p) {
    assert(pfn_valid(p) && pfa.meta[p].refcount && (pfa.meta[p].flags & FRAME_F_GUEST));
    pfa.meta[p].flags &= ~FRAME_F_GUEST;
}
int frame_is_lent_to_guest(pfn_t p) { return pfn_valid(p) && (pfa.meta[p].flags & FRAME_F_GUEST); }
void hyp_account_tables(int n) { table_count += n; assert(table_count >= 0); }
void hyp_account_mapped(int n) { mapped_count += n; assert(mapped_count >= 0); }

static uint64_t x86_leaf(pte_t prot) {
    return 5 | ((prot & PTE_W) ? 2 : 0) | ((prot & PTE_X) ? 0 : (1ULL << 63));
}
static uint64_t arm_leaf(pte_t prot) {
    return 3 | (15ULL << 2) | (3ULL << 8) | (1ULL << 10) |
           ((prot & PTE_R) ? (1ULL << 6) : 0) | ((prot & PTE_W) ? (1ULL << 7) : 0) |
           ((prot & PTE_X) ? 0 : (1ULL << 54));
}
static int invalidate(hyp_vm_t *vm, uint64_t gpa) {
    assert(vm->s2_lock.held); ++invalidations; last_invalidated = gpa; return invalidation_result;
}
static hyp_s2_format_t make_format(unsigned levels) {
    hyp_s2_format_t f = {
        .levels = levels, .address_mask = 0x0000fffffffff000ULL,
        .valid_mask = 1, .table_flags = levels == 4 ? 7 : 3,
        .leaf_flags = levels == 4 ? x86_leaf : arm_leaf,
        .invalidate = invalidate, .live_update = 0,
        .leaf_check_mask = levels == 4 ? ((1ULL << 63) | 3) : ((1ULL << 54) | 0xfff),
    };
    if (levels == 4) {
        f.address_mask = 0x000ffffffffff000ULL;
        f.leaf_check_mask = (1ULL << 63) | 7;
    } else {
        f.leaf_check_mask = 3 | (15ULL << 2) | (3ULL << 6) |
                            (3ULL << 8) | (1ULL << 54);
    }
    return f;
}
static uint64_t invalid_leaf(pte_t prot) { (void)prot; return 0; }
static uint64_t address_in_leaf(pte_t prot) { (void)prot; return 0x1001; }
static hyp_backend_t backend;
static hyp_vm_t init_vm(const hyp_s2_format_t *f) {
    hyp_vm_t vm; memset(&vm, 0, sizeof(vm));
    backend.gpa_limit = 1ULL << (12 + 9 * f->levels);
    vm.backend = &backend; vm.magic = HYP_VM_MAGIC;
    assert(hyp_s2_tree_init(&vm, f) == 0); return vm;
}
static pte_t *leaf_slot(hyp_vm_t *vm, uint64_t gpa) {
    pte_t *table = vm->s2_root;
    const hyp_s2_format_t *f = vm->s2_format;
    for (unsigned level = f->levels - 1; level; --level) {
        uint64_t e = table[(gpa >> (12 + 9 * level)) & 511];
        assert((e & ~f->address_mask) == f->table_flags);
        pfn_t p = phys_to_pfn(e & f->address_mask);
        assert(pfn_valid(p) && (pfa.meta[p].flags & FRAME_F_PT)); table = pfn_to_virt(p);
    }
    return &table[(gpa >> 12) & 511];
}
static void clean(void) {
    assert(table_count == 0 && mapped_count == 0 && live_frames == 0);
    assert(!pfa.lock.held);
}
static void test_format(unsigned levels) {
    hyp_s2_format_t f = make_format(levels);
    hyp_vm_t vm = init_vm(&f);
    assert(table_count == 1 && vm.s2_root_order == 0);
    pfn_t p = pfa_alloc_flags(0, 0), extra = pfa_alloc_flags(0, 0), out = PFN_NONE;
    uint64_t gpa = (1ULL << (12 + 9 * (levels - 1))) | 0x234000;
    unsigned before_invalidations = invalidations;
    assert(hyp_s2_tree_map(&vm, gpa, p, PTE_R | PTE_W | PTE_X) == 0);
    assert(table_count == (int)levels && mapped_count == 1);
    assert(invalidations == before_invalidations + 1 && last_invalidated == gpa);
    pte_t *leaf = leaf_slot(&vm, gpa);
    assert(*leaf == (pfn_to_phys(p) | f.leaf_flags(PTE_R | PTE_W | PTE_X)));
    /* Host mm_pt_prot_bits deliberately suppresses supervisor permissions.
     * Native stage-2 metadata must still record access without host PTE_U. */
    assert(mm_pt_prot_bits(PTE_R | PTE_W | PTE_X) == 0);
    assert(mm_pt_prot_bits(PTE_U | PTE_R | PTE_W) == (MM_ST_PROT_R | MM_ST_PROT_W));
    assert(pfa.meta[p].refcount == 1 && frame_is_lent_to_guest(p));
    assert(hyp_s2_tree_map(&vm, gpa, extra, PTE_R) == -EEXIST);
    assert(!frame_is_lent_to_guest(extra) && pfa.meta[extra].refcount == 1);
    assert(hyp_s2_tree_map(&vm, gpa + PAGE_SIZE, p, PTE_R) == -EBUSY);
    hyp_vm_t other = init_vm(&f);
    assert(hyp_s2_tree_map(&other, 0, p, PTE_R) == -EBUSY);
    hyp_s2_tree_destroy(&other);
    assert(frame_is_lent_to_guest(p) && pfa.meta[p].refcount == 1);
    assert(hyp_s2_tree_pin(&vm, gpa + 73, &out) == 0 && out == p);
    assert(pfa.meta[p].refcount == 2); frame_put(out);
    assert(hyp_s2_tree_translate(&vm, gpa + 73) == pfn_to_phys(p));
    assert(hyp_s2_tree_pin(&vm, gpa + PAGE_SIZE, &out) == -EFAULT);
    assert(hyp_s2_tree_pin(&vm, gpa, NULL) == -EINVAL);
    assert(hyp_s2_tree_map(&vm, gpa + 1, extra, PTE_R) == -EINVAL);
    assert(hyp_s2_tree_map(&vm, backend.gpa_limit, extra, PTE_R) == -ERANGE);
    assert(hyp_s2_tree_map(&vm, 0, PFN_NONE, PTE_R) == -EINVAL);
    uint64_t (*saved_flags)(pte_t) = f.leaf_flags;
    f.leaf_flags = invalid_leaf;
    assert(hyp_s2_tree_map(&vm, gpa + PAGE_SIZE, extra, PTE_R) == -EINVAL);
    f.leaf_flags = address_in_leaf;
    assert(hyp_s2_tree_map(&vm, gpa + PAGE_SIZE, extra, PTE_R) == -EINVAL);
    f.leaf_flags = saved_flags;
    assert(hyp_s2_tree_map(&vm, gpa + PAGE_SIZE, virt_to_pfn(vm.s2_root), PTE_R) == -EBUSY);
    vm.active_cpu_mask = 2;
    assert(hyp_s2_tree_map(&vm, gpa + PAGE_SIZE, extra, PTE_R) == -EBUSY);
    assert(hyp_s2_tree_unmap(&vm, gpa) == -EBUSY);
    vm.active_cpu_mask = 1; /* current CPU can update its own mapping */
    mm_pt_audit_report_t r;
    assert(hyp_s2_tree_audit(&vm, &r) == 0 && r.pt_pages == levels && r.entries == levels);
    uint64_t saved = *leaf;
    *leaf ^= levels == 4 ? 2 : (1ULL << 7);
    assert(hyp_s2_tree_audit(&vm, &r) == -EFAULT && r.prot_mismatch == 1); *leaf = saved;
    /* Hardware A/D updates are deliberately excluded from permission audits. */
    *leaf |= levels == 4 ? (3ULL << 5) : (1ULL << 51);
    assert(hyp_s2_tree_audit(&vm, &r) == 0); *leaf = saved;
    pfa.meta[p].flags &= ~FRAME_F_GUEST;
    assert(hyp_s2_tree_audit(&vm, &r) == -EFAULT && r.seg_extent_mismatch == 1);
    pfa.meta[p].flags |= FRAME_F_GUEST;
    assert(hyp_s2_tree_unmap(&vm, gpa + 1) == -EINVAL);
    assert(hyp_s2_tree_unmap(&vm, gpa) == 0);
    assert(!frame_is_lent_to_guest(p) && pfa.meta[p].refcount == 1 && mapped_count == 0);
    assert(hyp_s2_tree_translate(&vm, gpa) == 0);
    assert(hyp_s2_tree_unmap(&vm, gpa) == -ENOENT);
    assert(hyp_s2_tree_audit(&vm, &r) == 0);
    /* Last representable page walks every high radix index without wrapping. */
    uint64_t last = backend.gpa_limit - PAGE_SIZE;
    assert(hyp_s2_tree_map(&vm, last, extra, PTE_R) == 0);
    assert(hyp_s2_tree_pin(&vm, backend.gpa_limit - 1, &out) == 0 && out == extra);
    frame_put(out);
    assert(hyp_s2_tree_pin(&vm, backend.gpa_limit, &out) == -EFAULT);
    assert(hyp_s2_tree_translate(&vm, last + PAGE_SIZE - 1) == pfn_to_phys(extra));
    assert(hyp_s2_tree_audit(&vm, &r) == 0);
    frame_put(p); /* extra is now owned and released by VM destruction */
    hyp_s2_tree_destroy(&vm); assert(!vm.s2_root && !vm.s2_format); clean();
}
static void test_quarantine(unsigned levels) {
    hyp_s2_format_t f = make_format(levels);
    hyp_vm_t vm = init_vm(&f);
    pfn_t p = pfa_alloc_flags(0, 0);
    invalidation_result = -EIO;
    assert(hyp_s2_tree_map(&vm, 0, p, PTE_R) == 0);
    assert(vm.broken && frame_is_lent_to_guest(p) && mapped_count == 1);
    assert(hyp_s2_tree_translate(&vm, 0) == pfn_to_phys(p));
    assert(hyp_s2_tree_unmap(&vm, 0) == -EIO);
    assert(frame_is_lent_to_guest(p) && pfa.meta[p].refcount == 1 && mapped_count == 1);
    mm_pt_audit_report_t r; assert(hyp_s2_tree_audit(&vm, &r) == 0);
    invalidation_result = 0;
    /* A broken VM stays quarantined until backend detachment and teardown. */
    hyp_s2_tree_destroy(&vm); clean();
}
static void test_allocation_failure(unsigned levels) {
    hyp_s2_format_t f = make_format(levels);
    hyp_vm_t vm; memset(&vm, 0, sizeof(vm)); vm.backend = &backend;
    backend.gpa_limit = 1ULL << (12 + 9 * levels);
    alloc_budget = 0;
    assert(hyp_s2_tree_init(&vm, &f) == -ENOMEM && !vm.s2_root); clean();
    alloc_budget = -1; metadata_fail = 1;
    assert(hyp_s2_tree_init(&vm, &f) == -ENOMEM && !vm.s2_root); clean();
    metadata_fail = 0;
    vm = init_vm(&f);
    pfn_t p = pfa_alloc_flags(0, 0);
    alloc_budget = 1; /* partial path allocation must be reclaimable at teardown */
    assert(hyp_s2_tree_map(&vm, 0, p, PTE_R) == -ENOMEM);
    assert(!frame_is_lent_to_guest(p) && pfa.meta[p].refcount == 1 && mapped_count == 0);
    alloc_budget = -1;
    frame_put(p); hyp_s2_tree_destroy(&vm); clean();
}
static void test_rejected_formats(void) {
    hyp_s2_format_t f = make_format(3), bad;
    hyp_vm_t vm; memset(&vm, 0, sizeof(vm)); vm.backend = &backend;
    backend.gpa_limit = 1ULL << 39;
    assert(hyp_s2_tree_init(NULL, &f) == -EINVAL);
    assert(hyp_s2_tree_init(&vm, NULL) == -EINVAL);
#define BAD(member, value) do { bad = f; bad.member = (value); \
    assert(hyp_s2_tree_init(&vm, &bad) == -EINVAL); clean(); } while (0)
    BAD(levels, 1); BAD(levels, 5); BAD(leaf_flags, NULL); BAD(invalidate, NULL);
    BAD(valid_mask, 0); BAD(table_flags, 0); BAD(address_mask, 0);
    BAD(table_flags, f.table_flags | 0x1000);
    BAD(address_mask, f.address_mask | 1);
#undef BAD
    backend.gpa_limit = (1ULL << 39) + PAGE_SIZE;
    assert(hyp_s2_tree_init(&vm, &f) == -EINVAL); clean();
}
static void test_physical_address_bounds(void) {
    hyp_s2_format_t f = make_format(3);
    hyp_vm_t vm; memset(&vm, 0, sizeof(vm)); vm.backend = &backend;
    backend.gpa_limit = 1ULL << 39;
    physical_base = 1ULL << 50; /* root allocation exceeds descriptor PA mask */
    assert(hyp_s2_tree_init(&vm, &f) == -ERANGE && !vm.s2_root); clean();
    physical_base = 0x1fe000;
    f.address_mask = 0x1ff000;
    pfn_t p = pfa_alloc_flags(0, 0); /* 0x1fe000: guest and root fit the mask */
    vm = init_vm(&f);              /* 0x1ff000: next table cannot fit */
    assert(hyp_s2_tree_map(&vm, 0, p, PTE_R) == -ENOMEM);
    assert(table_count == 1 && mapped_count == 0 && !frame_is_lent_to_guest(p));
    frame_put(p); hyp_s2_tree_destroy(&vm); clean();
    physical_base = 0x12300000;
}
int main(void) {
    test_rejected_formats();
    test_physical_address_bounds();
    for (unsigned levels = 3; levels <= 4; ++levels) {
        test_format(levels); test_quarantine(levels); test_allocation_failure(levels);
    }
    puts("HYP_S2_HOST: PASS"); return 0;
}
