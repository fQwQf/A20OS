/* Run the real checked/present walkers against the old address-loop contract.
 * Only architecture tables and leaf transaction callbacks are modeled. The
 * unchanged Rust transaction has its own ownership/failure differential test. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define A20_COW_ANON_BATCH_PAGES 64
static int a20_cow_clone_anon_batch_rust(void *c, void *p, uintptr_t va, void *n) { (void)c; (void)p; (void)va; (void)n; return 0; }
#define ARCH_PT_ROOT_LEVEL 2
#define ARCH_HAS_PGTABLE_OPS 1
#define PTE_V 1UL
#define LEAF 2UL
#define SWAP 4UL
#define EEXIST 17
#define EAGAIN 11
#define ENOMEM 12
#define ROUND_DOWN(x,a) ((x)&~((a)-1))
#define ROUND_UP(x,a) (((x)+(a)-1)&~((a)-1))
typedef uintptr_t pte_t;
typedef uintptr_t vaddr_t;
enum { NPAGES=64, FULL=NPAGES*PAGE_SIZE };
typedef struct { uintptr_t va; int level, shared, swap; } Event;
typedef struct {
    pte_t *pgdir;
    unsigned char present[NPAGES], policy[NPAGES];
    Event events[NPAGES];
    size_t count, calls;
    uintptr_t fail_at;
} mm_struct_t;
static _Alignas(8) pte_t nodes[32][4];
static unsigned used;
static int swap_result;
static size_t hint_calls, hint_validations;
static size_t vm_pt_level_size(int level) { return PAGE_SIZE << (2*level); }
static int arch_pt_level_entries(int level) { (void)level; return 4; }
static int arch_pt_vpn(uintptr_t va,int level) { return (va/(PAGE_SIZE << (2*level)))&3; }
static int arch_pte_is_leaf(pte_t e) { return (e&LEAF)!=0; }
static pte_t *arch_pte_to_ptr(pte_t e) { return (pte_t *)(e&~7UL); }
#ifdef CONFIG_SWAP
static int pte_is_swap(pte_t e) { return (e&7)==SWAP; }
#endif
static pte_t probe(mm_struct_t *mm, uintptr_t va, int *level, uintptr_t *base) {
    pte_t *t=mm->pgdir;
    for(int l=ARCH_PT_ROOT_LEVEL;l>=0;l--) {
        pte_t e=t[arch_pt_vpn(va,l)];
        *level=l; *base=ROUND_DOWN(va,vm_pt_level_size(l));
        if(!(e&PTE_V)||arch_pte_is_leaf(e)||l==0) return e;
        t=arch_pte_to_ptr(e);
    }
    assert(0); return 0;
}
static int record(mm_struct_t *c,uintptr_t va,int level,int shared,int swap) {
    if(va==c->fail_at) return -ENOMEM;
    assert(c->count<NPAGES);
    c->events[c->count++]=(Event){va,level,shared,swap};
    size_t n=vm_pt_level_size(level)/PAGE_SIZE;
    for(size_t i=0;i<n;i++) c->present[va/PAGE_SIZE+i]=1;
    return 0;
}
int mm_clone_swap_page(mm_struct_t *c,uintptr_t va,mm_struct_t *p,uintptr_t src) {
    (void)p;assert(va==src);
    if(swap_result) return swap_result;
    if(c->present[va/PAGE_SIZE]) return -EEXIST;
    return record(c,va,0,0,1);
}
int a20_cow_clone_leaf_rust(mm_struct_t *c,mm_struct_t *p,uintptr_t va,int level,int shared) {
    (void)p;return record(c,va,level,shared,0);
}
int a20_cow_clone_leaf_at_rust(mm_struct_t *c,mm_struct_t *p,uintptr_t va,
                               int level,int shared,int checked,pte_t *slot,
                               pte_t *node) {
    hint_calls++;
    int index=arch_pt_vpn(va,level);
    assert(node!=NULL && index>=0 && index<arch_pt_level_entries(level));
    assert(slot==&node[index]);
    assert(*slot&PTE_V);
    assert(arch_pte_is_leaf(*slot));
    hint_validations++;
    if(checked) {
        if(p->policy[va/PAGE_SIZE]) return 0;
        if(c->present[va/PAGE_SIZE]) return 0;
    }
    return record(c,va,level,shared,0);
}
int mm_fork_clone_page(mm_struct_t *c,mm_struct_t *p,uintptr_t va,int shared) {
    c->calls++;
    if(p->policy[va/PAGE_SIZE]) return 0; /* DONTFORK/WIPEONFORK */
    int level;uintptr_t base;
    pte_t e=probe(p,va,&level,&base);
#ifdef CONFIG_SWAP
    if(pte_is_swap(e)) {
        int r=mm_clone_swap_page(c,va,p,va);
        return (r==-EEXIST||r==-EAGAIN)?0:r;
    }
#endif
    if(!(e&PTE_V)||!arch_pte_is_leaf(e)||va!=base||c->present[va/PAGE_SIZE]) return 0;
    return record(c,va,level,shared,0);
}

/* PRODUCTION */

static int reference_range(mm_struct_t *c,mm_struct_t *p,uintptr_t start,uintptr_t end,int shared) {
    start=ROUND_DOWN(start,PAGE_SIZE);end=ROUND_UP(end,PAGE_SIZE);
    for(uint64_t va=start;va<end;va+=PAGE_SIZE) {
        int r=mm_fork_clone_page(c,p,va,shared);if(r<0)return r;
    }
    return 0;
}
static int reference_present(mm_struct_t *c,mm_struct_t *p,pte_t *table,int level,
                             uintptr_t base,uintptr_t start,uintptr_t end,int shared) {
    if(!table||start>=end)return 0;
    size_t span=vm_pt_level_size(level);
    for(int i=0;i<4;i++) {
        uintptr_t a=base+i*span,b=a+span;
        if(b<=start)continue;if(a>=end)break;
        pte_t e=table[i];
#ifdef CONFIG_SWAP
        if(level==0&&pte_is_swap(e)) {
            int r=mm_clone_swap_page(c,a,p,a);
            if(r<0&&r!=-EEXIST&&r!=-EAGAIN)return r;
            continue;
        }
#endif
        if(!(e&PTE_V))continue;
        int r=0;
        if(arch_pte_is_leaf(e))r=a20_cow_clone_leaf_rust(c,p,a,level,shared);
        else if(level>0)r=reference_present(c,p,arch_pte_to_ptr(e),level-1,a,start,end,shared);
        if(r<0)return r;
    }
    return 0;
}
static uint32_t state=0x8124f55b;
static uint32_t random_u32(void) { state^=state<<13;state^=state>>17;state^=state<<5;return state; }
static pte_t *new_table(int level) {
    assert(used<32);pte_t *t=nodes[used++];
    for(int i=0;i<4;i++) {
        switch(random_u32()%5) {
        case 0:t[i]=0;break;
        case 1:case 2:t[i]=PTE_V|LEAF;break;
        default:t[i]=level?((uintptr_t)new_table(level-1)|PTE_V):SWAP;break;
        }
    }
    return t;
}
static void same(mm_struct_t *a,mm_struct_t *b) {
    assert(a->count==b->count);
    assert(!memcmp(a->present,b->present,sizeof(a->present)));
    for(size_t i=0;i<a->count;i++) {
        assert(a->events[i].va==b->events[i].va);
        assert(a->events[i].level==b->events[i].level);
        assert(a->events[i].shared==b->events[i].shared);
        assert(a->events[i].swap==b->events[i].swap);
    }
}
int main(void) {
    mm_struct_t p={.pgdir=nodes[0]},a={0},b={0};
    a.fail_at=b.fail_at=UINTPTR_MAX;
    assert(mm_fork_clone_range(&a,&p,0,FULL,0)==0);
    assert(reference_range(&b,&p,0,FULL,0)==0);
    assert(a.calls==0&&b.calls==NPAGES); /* Skip an entire absent subtree. */
    for(int tree=0;tree<1000;tree++) {
        memset(nodes,0,sizeof(nodes));used=0;p.pgdir=new_table(ARCH_PT_ROOT_LEVEL);
        for(int i=0;i<NPAGES;i++)p.policy[i]=(random_u32()%8==0);
        for(int trial=0;trial<200;trial++) {
            memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));
            for(int i=0;i<NPAGES;i++)a.present[i]=b.present[i]=(random_u32()%7==0);
            a.fail_at=b.fail_at=(trial%3)?UINTPTR_MAX:(random_u32()%NPAGES)*PAGE_SIZE;
            uintptr_t start=random_u32()%(FULL+1),end=random_u32()%(FULL+1);
            if(trial%5==0) {start=ROUND_DOWN(start,PAGE_SIZE);end=FULL;}
            int shared=trial%2;
            swap_result=(trial%4==0)?-EEXIST:(trial%4==1)?-EAGAIN:0;
            int ar=mm_fork_clone_range(&a,&p,start,end,shared);
            int br=reference_range(&b,&p,start,end,shared);
            assert(ar==br);same(&a,&b);
            /* Existing VMA walker keeps its partial-huge and swap semantics. */
            memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));
            a.fail_at=b.fail_at=UINTPTR_MAX;
            ar=mm_fork_clone_present_range(&a,&p,start,end,shared);
            br=reference_present(&b,&p,p.pgdir,ARCH_PT_ROOT_LEVEL,0,
                                 ROUND_DOWN(start,PAGE_SIZE),ROUND_UP(end,PAGE_SIZE),shared);
            assert(ar==br);same(&a,&b);
        }
    }
    puts("fork range real C walkers PASS: 200000 randomized ranges, policy/dst/huge/swap/errors");
    return 0;
}
