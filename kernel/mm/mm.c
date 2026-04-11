#include "defs.h"
#include "mm.h"
#include "panic.h"
#include "stdio.h"
#include "string.h"

#define TOTAL_FRAMES ((PHYS_MEMORY_END - PHYS_MEMORY_BASE) / PAGE_SIZE)

static uint8_t frame_bitmap[TOTAL_FRAMES / 8];
static size_t used_frames;

static char kernel_heap[KERNEL_HEAP_SIZE];
char *heap_ptr = kernel_heap;

struct heap_block {
    size_t size;
    int free;
    struct heap_block *next;
};

static struct heap_block *heap_list = NULL;

#define FRAME_INDEX(pa)  (((paddr_t)(pa) - PHYS_MEMORY_BASE) >> PAGE_SIZE_BITS)
#define FRAME_ADDR(idx)  (PHYS_MEMORY_BASE + ((size_t)(idx) << PAGE_SIZE_BITS))
#define BITMAP_SET(idx)  (frame_bitmap[(idx)/8] |=  (1 << ((idx)%8)))
#define BITMAP_CLR(idx)  (frame_bitmap[(idx)/8] &= ~(1 << ((idx)%8)))
#define BITMAP_TST(idx)  (frame_bitmap[(idx)/8] &   (1 << ((idx)%8)))

#define PTE_FROM_PA(pa)  (((uint64_t)(pa) >> 12) << 10)

void mm_init(void) {
    memset(frame_bitmap, 0, sizeof(frame_bitmap));
    used_frames = 0;

    extern char _bss_end[];
    size_t kernel_end_idx = FRAME_INDEX(ROUND_UP((paddr_t)(uintptr_t)_bss_end, PAGE_SIZE));
    if (kernel_end_idx > TOTAL_FRAMES) kernel_end_idx = TOTAL_FRAMES;
    for (size_t i = 0; i < kernel_end_idx; i++) {
        BITMAP_SET(i);
        used_frames++;
    }

    heap_ptr = kernel_heap;
    heap_list = NULL;
    printf("[MM] %d frames, %d free, heap %d KB\n",
           (int)TOTAL_FRAMES, (int)(TOTAL_FRAMES - used_frames),
           KERNEL_HEAP_SIZE / 1024);
}

void *frame_alloc(void) {
    for (size_t i = 0; i < TOTAL_FRAMES; i++) {
        if (!BITMAP_TST(i)) {
            BITMAP_SET(i);
            used_frames++;
            paddr_t pa = FRAME_ADDR(i);
            memset((void *)pa, 0, PAGE_SIZE);
            return (void *)pa;
        }
    }
    panic("frame_alloc: out of frames");
    return NULL;
}

void frame_free(void *addr) {
    paddr_t pa = (paddr_t)addr;
    if (pa < PHYS_MEMORY_BASE || pa >= PHYS_MEMORY_END) return;
    size_t idx = FRAME_INDEX(pa);
    if (idx < TOTAL_FRAMES && BITMAP_TST(idx)) {
        BITMAP_CLR(idx);
        used_frames--;
    }
}

size_t frame_free_count(void) {
    return TOTAL_FRAMES - used_frames;
}

void heap_init(void) {
    heap_ptr = kernel_heap;
    heap_list = NULL;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 15) & ~15UL;

    if (heap_list != NULL) {
        struct heap_block *blk = heap_list;
        while (blk) {
            if (blk->free && blk->size >= size) {
                blk->free = 0;
                return (void *)((char *)blk + sizeof(struct heap_block));
            }
            blk = blk->next;
        }
    }

    if (heap_ptr + sizeof(struct heap_block) + size > kernel_heap + KERNEL_HEAP_SIZE) {
        panic("kmalloc: out of heap memory");
    }

    struct heap_block *blk = (struct heap_block *)heap_ptr;
    blk->size = size;
    blk->free = 0;
    blk->next = heap_list;
    heap_list = blk;
    heap_ptr += sizeof(struct heap_block) + size;

    return (void *)((char *)blk + sizeof(struct heap_block));
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct heap_block *blk = (struct heap_block *)((char *)ptr - sizeof(struct heap_block));
    blk->free = 1;
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    struct heap_block *blk = (struct heap_block *)((char *)ptr - sizeof(struct heap_block));
    size_t old_size = blk->size;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    kfree(ptr);
    return new_ptr;
}

void *kcalloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

uint64_t *pt_create(void) {
    return (uint64_t *)frame_alloc();
}

void pt_destroy(uint64_t *pgdir) {
    if (!pgdir) return;
    for (int i = 0; i < 512; i++) {
        uint64_t pte = pgdir[i];
        if ((pte & PTE_V) && !(pte & PTE_R) && !(pte & PTE_W) && !(pte & PTE_X)) {
            uint64_t *next = (uint64_t *)SV39_PTE_ADDR(pte);
            pt_destroy(next);
        }
    }
    frame_free(pgdir);
}

uint64_t *pt_walk(uint64_t *pgdir, vaddr_t va, int alloc) {
    uint64_t *table = pgdir;
    for (int level = 2; level > 0; level--) {
        int vpn = SV39_VPN(va, level);
        uint64_t pte = table[vpn];
        if (pte & PTE_V) {
            table = (uint64_t *)SV39_PTE_ADDR(pte);
        } else {
            if (!alloc) return NULL;
            uint64_t *next = (uint64_t *)frame_alloc();
            if (!next) return NULL;
            memset(next, 0, PAGE_SIZE);
            table[vpn] = PTE_FROM_PA((uint64_t)(uintptr_t)next) | PTE_V;
            table = next;
        }
    }
    return &table[SV39_VPN(va, 0)];
}

int pt_map(uint64_t *pgdir, vaddr_t va, paddr_t pa, uint64_t flags) {
    uint64_t *pte = pt_walk(pgdir, va, 1);
    if (!pte) return -ENOMEM;
    *pte = PTE_FROM_PA(pa) | flags | PTE_V;
    return 0;
}

int pt_unmap(uint64_t *pgdir, vaddr_t va) {
    uint64_t *pte = pt_walk(pgdir, va, 0);
    if (!pte || !(*pte & PTE_V)) return -EINVAL;
    *pte = 0;
    return 0;
}

paddr_t pt_translate(uint64_t *pgdir, vaddr_t va) {
    uint64_t *pte = pt_walk(pgdir, va, 0);
    if (!pte || !(*pte & PTE_V)) return 0;
    return SV39_PTE_ADDR(*pte) | (va & 0xFFF);
}

/* ---- Per-process page table helpers ---- */

static void set_megapage(uint64_t *pgdir, uint64_t va, uint64_t pa, uint64_t flags) {
    int vpn2 = (va >> 30) & 0x1FF;
    if (!(pgdir[vpn2] & PTE_V)) {
        uint64_t *l1 = (uint64_t *)frame_alloc();
        if (!l1) panic("set_megapage: frame_alloc failed for L1 table");
        memset(l1, 0, PAGE_SIZE);
        pgdir[vpn2] = PTE_FROM_PA((uint64_t)(uintptr_t)l1) | PTE_V;
    }
    uint64_t *l1tbl = (uint64_t *)(uintptr_t)SV39_PTE_ADDR(pgdir[vpn2]);
    int vpn1 = (va >> 21) & 0x1FF;
    l1tbl[vpn1] = PTE_FROM_PA(pa) | flags;
}

void pt_map_kernel(uint64_t *pgdir) {
    uint64_t kflags = PTE_KERN | PTE_A | PTE_D;

    for (uint64_t pa = PHYS_MEMORY_BASE; pa < PHYS_MEMORY_END; pa += (2UL << 20))
        set_megapage(pgdir, pa, pa, kflags);

    set_megapage(pgdir, CLINT_BASE, CLINT_BASE, kflags);
    set_megapage(pgdir, PLIC_BASE, PLIC_BASE, kflags);
    set_megapage(pgdir, 0x0C200000UL, 0x0C200000UL, kflags);
    set_megapage(pgdir, UART0_BASE, UART0_BASE, kflags);
}

int pt_map_range(uint64_t *pgdir, vaddr_t va, paddr_t pa, size_t size, uint64_t flags) {
    size = ROUND_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        int r = pt_map(pgdir, va + off, pa + off, flags);
        if (r < 0) return r;
    }
    return 0;
}

static uint64_t *pt_clone_level(uint64_t *src, int level) {
    uint64_t *dst = (uint64_t *)frame_alloc();
    if (!dst) return NULL;
    memset(dst, 0, PAGE_SIZE);

    for (int i = 0; i < 512; i++) {
        uint64_t pte = src[i];
        if (!(pte & PTE_V)) continue;

        int is_leaf = (pte & PTE_R) || (pte & PTE_W) || (pte & PTE_X);

        if (is_leaf) {
            if (pte & PTE_U) {
                void *nf = frame_alloc();
                if (!nf) { pt_destroy(dst); return NULL; }
                memcpy(nf, (void *)(uintptr_t)SV39_PTE_ADDR(pte), PAGE_SIZE);
                dst[i] = PTE_FROM_PA((uint64_t)(uintptr_t)nf) | (pte & 0x3FF);
            } else {
                dst[i] = pte;
            }
        } else {
            uint64_t *next_src = (uint64_t *)(uintptr_t)SV39_PTE_ADDR(pte);
            uint64_t *next_dst = pt_clone_level(next_src, level - 1);
            if (!next_dst) { pt_destroy(dst); return NULL; }
            dst[i] = PTE_FROM_PA((uint64_t)(uintptr_t)next_dst) | PTE_V;
        }
    }
    return dst;
}

uint64_t *pt_clone(uint64_t *src_pgdir) {
    if (!src_pgdir) return NULL;
    return pt_clone_level(src_pgdir, 2);
}

static void pt_destroy_user_recursive(uint64_t *table) {
    if (!table) return;
    for (int i = 0; i < 512; i++) {
        uint64_t pte = table[i];
        if (!(pte & PTE_V)) continue;

        int is_leaf = (pte & PTE_R) || (pte & PTE_W) || (pte & PTE_X);

        if (is_leaf) {
            if (pte & PTE_U)
                frame_free((void *)(uintptr_t)SV39_PTE_ADDR(pte));
        } else {
            uint64_t *next = (uint64_t *)(uintptr_t)SV39_PTE_ADDR(pte);
            pt_destroy_user_recursive(next);
            frame_free(next);
        }
    }
}

void pt_destroy_user(uint64_t *pgdir) {
    if (!pgdir) return;
    pt_destroy_user_recursive(pgdir);
    frame_free(pgdir);
}
