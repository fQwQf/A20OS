/* Execute actual C snapshot/downgrade/unlock/ref-drop adapters. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define PMD_SIZE (2UL*1024*1024)
#define PFN_NONE UINT32_MAX
#define VM_VMO 16
#define PTE_V 1UL
#define PTE_W 2UL
#define PTE_COW 4UL
#define PTE_LEAF 8UL
typedef uintptr_t pte_t;
typedef uintptr_t paddr_t;
typedef uint32_t pfn_t;
typedef struct { pte_t *pgdir; } mm_struct_t;
typedef struct { unsigned vm_flags; } mm_seg_t;
typedef struct { int pin; } page_cache_page_t;
static pte_t tables[2][512];
static mm_seg_t vma;
static page_cache_page_t cache;
static int active,locked=-1,leaf_walks,node_walks,locks,unlocks;
static int frame_gets,frame_puts,cache_puts,cache_mode,present_vma=1,statuses,changes;
static size_t changed_size;
static pte_t *pt_lookup_leaf(pte_t *pgdir,uintptr_t va,void *l,void *b,void *sz) {
    (void)pgdir;(void)va;(void)l;(void)b;(void)sz;leaf_walks++;
    return &tables[active][0];
}
static pte_t *mm_pt_leaf_table(pte_t *pgdir,uintptr_t va) {
    (void)pgdir;(void)va;node_walks++;return tables[active];
}
static void mm_pt_node_lock(pte_t *table) {
    assert(locked==-1);locked=table==tables[0]?0:1;locks++;
}
static void mm_pt_node_unlock(pte_t *table) {
    assert(locked==(table==tables[0]?0:1));locked=-1;unlocks++;
}
static int arch_pte_is_leaf(pte_t e) {return (e&PTE_LEAF)!=0;}
static uintptr_t arch_pte_addr(pte_t e) {return e&~4095UL;}
static pte_t arch_pte_flags(pte_t e) {return e&4095;}
static pte_t mm_cow_flags(pte_t e) {return (arch_pte_flags(e)&~PTE_W)|PTE_COW;}
static pfn_t phys_to_pfn(uintptr_t pa) {return pa/4096;}
static mm_seg_t *mm_seg_find(mm_struct_t *mm,uintptr_t va) {
    (void)mm;(void)va;return present_vma?&vma:NULL;
}
static unsigned char mm_fork_page_class(mm_struct_t *mm,uintptr_t va) {
    (void)mm;(void)va;return 7;
}
static page_cache_page_t *mm_file_cache_mapping_get(mm_seg_t *v,uintptr_t va,pfn_t pfn) {
    (void)v;(void)va;assert(locked==0&&pfn==9);
    if(cache_mode){cache.pin++;return &cache;}return NULL;
}
static void frame_get(pfn_t pfn) {assert(locked==0&&pfn==9);frame_gets++;}
static void frame_put(pfn_t pfn) {assert(locked==-1&&pfn==9);frame_puts++;}
static void page_cache_put(page_cache_page_t *p) {assert(locked==-1&&p==&cache);cache.pin--;cache_puts++;}
static size_t vm_pt_level_size(int level) {return level?PMD_SIZE:PAGE_SIZE;}
static pte_t arch_pte_leaf(paddr_t pa,pte_t flags) {return pa|flags|PTE_LEAF;}
static pte_t arch_pte_block(paddr_t pa,pte_t flags) {return pa|flags|PTE_LEAF|0x100;}
static void mm_tlb_note_change(mm_struct_t *mm,uintptr_t va,size_t size) {
    (void)mm;assert(locked==0&&va==0);changes++;changed_size=size;
}
static int arch_pt_vpn(uintptr_t va,int level) {(void)va;(void)level;return 0;}
static int mm_pt_sync_status(pte_t *table,int level,int idx,unsigned char cls) {
    assert(locked==0&&table==tables[0]&&(level==0||level==1)&&idx==0&&cls==7);
    statuses++;return 0;
}
/* SNAPSHOT_STRUCT */
/* SNAPSHOT_FUNCTIONS */
/* RELEASE_FUNCTION */
static void reset(void) {
    assert(locked==-1);memset(tables,0,sizeof(tables));
    tables[0][0]=0x9000|PTE_V|PTE_W|PTE_LEAF;
    active=leaf_walks=node_walks=locks=unlocks=frame_gets=frame_puts=cache_puts=statuses=changes=0;
    cache_mode=0;cache.pin=0;vma.vm_flags=0;present_vma=1;
}
int main(void) {
    mm_struct_t parent={tables[0]};struct a20_cow_shared_snapshot snap;
    for(int level=0;level<2;level++)for(int kind=0;kind<3;kind++) {
        reset();if(kind==1)cache_mode=1;if(kind==2)vma.vm_flags=VM_VMO;
        assert(a20_cow_share_snapshot(&parent,0,level,&snap)==1);
        assert(snap.slot==&tables[0][0]&&snap.node==tables[0]&&snap.pfn==9);
        assert(locked==0&&snap.kind==(kind==1?2:kind==2?0:1));
        assert(frame_gets==(kind==0)&&cache.pin==(kind==1));
        assert(leaf_walks==1&&node_walks==1);
        /* Even if a fresh root lookup now sees another node, the held token
         * names the original lock and slot. No helper may re-walk it. */
        active=1;
        a20_cow_share_downgrade(&parent,0,level,&snap,snap.cow_flags);
        assert((tables[0][0]&PTE_COW)&&!(tables[0][0]&PTE_W));
        assert(tables[1][0]==0&&changes==1&&changed_size==vm_pt_level_size(level)&&statuses==1);
        a20_cow_share_unlock(&snap);
        assert(locked==-1&&locks==1&&unlocks==1&&leaf_walks==1&&node_walks==1);
        a20_cow_release_ref(&snap);
        assert(frame_puts==(kind==0)&&cache_puts==(kind==1)&&cache.pin==0);
    }
    for(int valid=0;valid<2;valid++) {
        reset();tables[0][0]=valid?PTE_V:0;
        assert(a20_cow_share_snapshot(&parent,0,0,&snap)==0);
        assert(locked==-1&&locks==1&&unlocks==1&&frame_gets==0&&cache.pin==0);
        assert(snap.slot==NULL&&snap.node==NULL);
    }
    reset();present_vma=0;
    assert(a20_cow_share_snapshot(&parent,0,0,&snap)==1&&frame_gets==1);
    a20_cow_share_unlock(&snap);a20_cow_release_ref(&snap);assert(frame_puts==1);
    puts("COW_SNAPSHOT_TOKEN: real C adapters PASS slot/node identity, refs, huge/status and vanished leaf");
    return 0;
}
