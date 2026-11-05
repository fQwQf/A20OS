/* Execute actual C snapshot/downgrade/unlock/ref-drop adapters. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define PMD_SIZE (2UL*1024*1024)
#define ARCH_PT_ROOT_LEVEL 1
#define PFN_NONE UINT32_MAX
#define VM_VMO 16
#define VM_FILE 32
#define VM_SHARED 64
#define VM_ANON 128
#define VM_PFNMAP 256
#define VM_DONTFORK 512
#define VM_WIPEONFORK 1024
#define VM_EXEC 2048
#define MM_SAFE_UFFD 1
#define MM_SAFE_NO_FA 2
#define MM_SAFE_MASK 3
#define A20_COW_ANON_BATCH_PAGES 16
/* CLASS_DEFINITIONS */
#define PTE_V 1UL
#define PTE_W 2UL
#define PTE_COW 4UL
#define PTE_LEAF 8UL
#define PTE_X 16UL
typedef uintptr_t pte_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;
typedef uint32_t pfn_t;
typedef struct { pte_t *pgdir; } mm_struct_t;
typedef struct { unsigned vm_flags; uintptr_t start, end; void *file, *file_vnode; } mm_seg_t;
typedef struct { int pin; } page_cache_page_t;
typedef struct { uint8_t cls[512], safe[64]; page_cache_page_t **cache_owner; } pt_meta_t;
struct a20_cow_anon_batch_page { uint32_t pfn, class; uintptr_t flags; void *pcp; uint32_t kind; };
static pt_meta_t metadata;
static page_cache_page_t *cache_owners[512];
static int private_copy;
static pfn_t cache_pfn;
static int arch_fork_requires_private_copy(void) { return private_copy; }
static pt_meta_t *mm_pt_meta(pte_t *table) { (void)table; return &metadata; }
static int mm_pt_meta_level(pt_meta_t *meta) { (void)meta; return 0; }
static int pfn_valid(pfn_t pfn) { return pfn == 9; }
static int cow_child_batch_empty(mm_struct_t *mm, uintptr_t va, size_t nr, int first) {
    (void)mm; (void)va; (void)nr; (void)first; return 1;
}
static pte_t tables[2][512];
static mm_seg_t vma;
static page_cache_page_t cache;
static int active,locked=-1,leaf_walks,node_walks,locks,unlocks;
static int frame_gets,frame_puts,cache_puts,cache_mode,present_vma=1,statuses,changes;
static size_t changed_size;
static unsigned char stored_class, expected_class;
static int status_reads, vma_lookups, expected_level=-1, expected_index;
static uintptr_t expected_va;
static int vanish_on_lock;
static pte_t *pt_lookup_leaf(pte_t *pgdir,uintptr_t va,void *l,void *b,void *sz) {
    (void)pgdir;(void)l;(void)b;(void)sz;leaf_walks++;expected_va=va;
    return &tables[active][expected_index];
}
static pte_t *mm_pt_leaf_table(pte_t *pgdir,uintptr_t va) {
    (void)pgdir;(void)va;node_walks++;return tables[active];
}
static void mm_pt_node_lock(pte_t *table) {
    assert(locked==-1);locked=table==tables[0]?0:1;locks++;
    if(vanish_on_lock) { tables[locked][expected_index]=0; vanish_on_lock=0; }
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
    (void)mm;(void)va;vma_lookups++;return present_vma?&vma:NULL;
}
static int arch_pt_vpn(uintptr_t va,int level);
static uint8_t mm_pt_peek(pte_t *table,int level,int idx) {
    assert(locked==0&&table==tables[0]&&(level==0||level==1)&&idx==expected_index);
    if(expected_level>=0)assert(level==expected_level);
    status_reads++;return MM_ST_CLS_BYTE(stored_class)|7;
}
/* CLASS_HELPER */
static page_cache_page_t *mm_pt_cache_peek(pte_t *table,int idx) {
    assert(locked==0&&table==tables[0]&&idx==expected_index);
    return metadata.cache_owner ? metadata.cache_owner[idx] : NULL;
}
static page_cache_page_t *mm_file_cache_mapping_get(mm_seg_t *v,uintptr_t va,pfn_t pfn) {
    (void)v;(void)va;assert(locked==0&&pfn==9);
    if(cache_mode){cache.pin++;return &cache;}return NULL;
}
static pfn_t page_cache_pfn(page_cache_page_t *page) {
    assert(page==&cache);return cache_pfn;
}
static void page_cache_pin(page_cache_page_t *page) {
    assert(page==&cache);cache.pin++;
}
static void frame_get(pfn_t pfn) {assert(locked==0&&pfn==9);frame_gets++;}
static void frame_get_many(const pfn_t *pfns, size_t count) {
    for (size_t i=0; i<count; i++) frame_get(pfns[i]);
}
static void frame_put(pfn_t pfn) {assert(locked==-1&&pfn==9);frame_puts++;}
static void page_cache_put(page_cache_page_t *p) {assert(locked==-1&&p==&cache);cache.pin--;cache_puts++;}
static size_t vm_pt_level_size(int level) {return level?PMD_SIZE:PAGE_SIZE;}
static pte_t arch_pte_leaf(paddr_t pa,pte_t flags) {return pa|flags|PTE_LEAF;}
static pte_t arch_pte_block(paddr_t pa,pte_t flags) {return pa|flags|PTE_LEAF|0x100;}
static void mm_tlb_note_change(mm_struct_t *mm,uintptr_t va,size_t size) {
    (void)mm;assert(locked==0&&va==expected_va);changes++;changed_size=size;
}
static int arch_pt_vpn(uintptr_t va,int level) {
    assert(level>=0&&level<=ARCH_PT_ROOT_LEVEL);
    return (va>>(12+9*level))&511;
}
static int arch_pt_level_entries(int level) {return level>=0&&level<2?512:0;}
static int mm_pt_sync_status(pte_t *table,int level,int idx,unsigned char cls) {
    assert(locked==0&&table==tables[0]&&(level==0||level==1)&&idx==expected_index&&cls==expected_class);
    statuses++;return 0;
}
static int mm_pt_sync_status_meta(pt_meta_t *meta,int idx,unsigned char cls,pte_t pte) {
    (void)meta;(void)idx;(void)cls;(void)pte;
    return 0;
}
/* SNAPSHOT_STRUCT */
/* SNAPSHOT_FUNCTIONS */
/* RELEASE_FUNCTION */
static void reset(void) {
    assert(locked==-1);memset(tables,0,sizeof(tables));
    tables[0][0]=0x9000|PTE_V|PTE_W|PTE_LEAF;
    active=leaf_walks=node_walks=locks=unlocks=frame_gets=frame_puts=cache_puts=statuses=changes=0;
    cache_mode=0;cache.pin=0;vma.vm_flags=0;present_vma=1;
    vma.file=vma.file_vnode=NULL;private_copy=0;cache_pfn=9;
    memset(&metadata,0,sizeof(metadata));memset(cache_owners,0,sizeof(cache_owners));
    stored_class=expected_class=MM_ST_FILE_PRIVATE;status_reads=vma_lookups=0;
    expected_level=-1;expected_index=0;expected_va=0;vanish_on_lock=0;
}
int main(void) {
    mm_struct_t parent={tables[0]};struct a20_cow_shared_snapshot snap;
    for(int level=0;level<2;level++)for(int kind=0;kind<3;kind++) {
        reset();if(kind==1){cache_mode=1;vma.vm_flags=VM_FILE;}if(kind==2)vma.vm_flags=VM_VMO;
        expected_index=0;expected_va=0;
        tables[0][0]=0x9000|PTE_V|PTE_W|PTE_LEAF;
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
    /* The walker-supplied entry must use the same token/ref transaction but
     * perform no root lookup. Use nonzero VPNs at both leaf geometries. */
    for(int level=0;level<2;level++)for(int kind=0;kind<3;kind++) {
        reset();
        expected_index=level?2:7;
        expected_va=(uintptr_t)expected_index*vm_pt_level_size(level);
        if(kind==1){cache_mode=1;vma.vm_flags=VM_FILE;}
        if(kind==2)vma.vm_flags=VM_VMO;
        tables[0][expected_index]=0x9000|PTE_V|PTE_W|PTE_LEAF;
        assert(arch_pt_vpn(expected_va,level)==expected_index);
        assert(a20_cow_share_snapshot_at(&parent,expected_va,level,
                    &tables[0][expected_index],tables[0],&snap)==1);
        assert(snap.slot==&tables[0][expected_index]&&snap.node==tables[0]&&snap.pfn==9);
        assert(locked==0&&snap.kind==(kind==1?2:kind==2?0:1));
        assert(frame_gets==(kind==0)&&cache.pin==(kind==1));
        assert(leaf_walks==0&&node_walks==0);
        active=1;
        a20_cow_share_downgrade(&parent,expected_va,level,&snap,snap.cow_flags);
        assert(tables[0][expected_index]&PTE_COW);
        assert(tables[1][expected_index]==0&&changes==1&&
               changed_size==vm_pt_level_size(level)&&statuses==1);
        a20_cow_share_unlock(&snap);
        assert(locked==-1&&locks==1&&unlocks==1&&leaf_walks==0&&node_walks==0);
        a20_cow_release_ref(&snap);
        assert(frame_puts==(kind==0)&&cache_puts==(kind==1)&&cache.pin==0);
    }
    /* Invalid borrowed geometry is rejected before taking a node lock or
     * acquiring any backing reference. */
    reset();
    tables[0][0]=0x9000|PTE_V|PTE_W|PTE_LEAF;
    assert(a20_cow_share_snapshot_at(&parent,0,0,&tables[0][0],tables[1],&snap)==0);
    assert(locked==-1&&locks==0&&unlocks==0&&frame_gets==0&&cache.pin==0);
    assert(snap.slot==NULL&&snap.node==NULL&&snap.pfn==PFN_NONE);
    assert(a20_cow_share_snapshot_at(&parent,0,0,NULL,tables[0],&snap)==0);
    assert(locked==-1&&locks==0&&frame_gets==0);
    assert(snap.slot==NULL&&snap.node==NULL&&snap.pfn==PFN_NONE);
    for(int bad_level=-1;bad_level<=ARCH_PT_ROOT_LEVEL+1;bad_level+=ARCH_PT_ROOT_LEVEL+2) {
        assert(a20_cow_share_snapshot_at(&parent,0,bad_level,
                    &tables[0][0],tables[0],&snap)==0);
        assert(locked==-1&&locks==0&&unlocks==0&&frame_gets==0&&cache.pin==0);
        assert(snap.slot==NULL&&snap.node==NULL&&snap.pfn==PFN_NONE);
    }
    /* The source may disappear between walker traversal and node exclusion;
     * the snapshot re-reads under lock and releases that lock on rejection. */
    reset();
    expected_index=7;expected_va=7*PAGE_SIZE;
    tables[0][expected_index]=0x9000|PTE_V|PTE_W|PTE_LEAF;
    vanish_on_lock=1;
    assert(a20_cow_share_snapshot_at(&parent,expected_va,0,
                &tables[0][expected_index],tables[0],&snap)==0);
    assert(locked==-1&&locks==1&&unlocks==1&&frame_gets==0&&cache.pin==0);
    assert(snap.slot==NULL&&snap.node==NULL&&leaf_walks==0&&node_walks==0);
    for(int valid=0;valid<2;valid++) {
        reset();tables[0][0]=valid?PTE_V:0;
        assert(a20_cow_share_snapshot(&parent,0,0,&snap)==0);
        assert(locked==-1&&locks==1&&unlocks==1&&frame_gets==0&&cache.pin==0);
        assert(snap.slot==NULL&&snap.node==NULL);
    }
    struct ClassCase { unsigned char stored;unsigned flags;int present;unsigned char expected; };
    const struct ClassCase cases[]={
        {MM_ST_FILE_SHARED,VM_FILE|VM_SHARED,1,MM_ST_FILE_SHARED},
        {MM_ST_FILE_PRIVATE,VM_VMO,1,MM_ST_FILE_PRIVATE},
        {MM_ST_INVALID,VM_VMO,1,MM_ST_VMO},
        {MM_ST_PT_NODE,VM_FILE|VM_SHARED,1,MM_ST_FILE_SHARED},
        {MM_ST_INVALID,VM_FILE,1,MM_ST_FILE_PRIVATE},
        {MM_ST_INVALID,0,0,MM_ST_ANON_MAPPED},
    };
    for(int level=0;level<2;level++)for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        reset();stored_class=cases[i].stored;expected_class=cases[i].expected;
        vma.vm_flags=cases[i].flags;present_vma=cases[i].present;
        assert(a20_cow_share_snapshot(&parent,0,level,&snap)==1);
        assert(snap.class==expected_class&&status_reads==1&&vma_lookups==1);
        active=1;
        a20_cow_share_downgrade(&parent,0,level,&snap,snap.cow_flags);
        a20_cow_share_unlock(&snap);a20_cow_release_ref(&snap);
        assert(leaf_walks==1&&node_walks==1&&status_reads==1&&vma_lookups==1);
    }
    reset();
    assert(a20_cow_share_snapshot(&parent,0,0,&snap)==1);
    expected_level=0;expected_index=6;stored_class=MM_ST_FILE_PRIVATE;
    assert(mm_fork_page_class(tables[0],0,0x206000,&vma)==MM_ST_FILE_PRIVATE);
    expected_level=1;expected_index=1;stored_class=MM_ST_FILE_SHARED;
    assert(mm_fork_page_class(tables[0],1,0x206000,&vma)==MM_ST_FILE_SHARED);
    a20_cow_share_unlock(&snap);a20_cow_release_ref(&snap);
    reset();present_vma=0;
    assert(a20_cow_share_snapshot(&parent,0,0,&snap)==1&&frame_gets==1);
    a20_cow_share_unlock(&snap);a20_cow_release_ref(&snap);assert(frame_puts==1);
    reset();vma.start=0;vma.end=PAGE_SIZE;
    assert(a20_cow_share_snapshot_at_vma(&parent,0,0,&tables[0][0],tables[0],
                                         &vma,&snap)==1);
    assert(vma_lookups==0&&frame_gets==1&&leaf_walks==0&&node_walks==0);
    a20_cow_share_unlock(&snap);a20_cow_release_ref(&snap);
    reset();vma.start=PAGE_SIZE;vma.end=2*PAGE_SIZE;
    assert(a20_cow_share_snapshot_at_vma(&parent,0,0,&tables[0][0],tables[0],
                                         &vma,&snap)==0);
    assert(vma_lookups==0&&frame_gets==0&&locks==1&&unlocks==1&&locked==-1);
    /* The elision proof is cache ownership, never read-only permission. */
    reset(); vma.vm_flags=VM_FILE;vma.file=vma.file_vnode=&vma;
    vma.start=0;vma.end=1024*1024;metadata.cache_owner=cache_owners;
    for(int i=0;i<16;i++) {
        metadata.cls[i]=MM_ST_CLS_BYTE(MM_ST_FILE_PRIVATE);
        cache_owners[i]=&cache;
        tables[0][i]=0x9000|PTE_V|PTE_LEAF|PTE_COW;
    }
    pte_t clean=tables[0][0];
    assert(cow_file_leaf_refaultable(&vma,&metadata,0,0,clean));
    vma.end=1024*1024-PAGE_SIZE;
    assert(!cow_file_leaf_refaultable(&vma,&metadata,0,0,clean));vma.end=1024*1024;
    vma.vm_flags|=VM_EXEC;
    assert(!cow_file_leaf_refaultable(&vma,&metadata,0,0,clean|PTE_X));
    vma.vm_flags&=~VM_EXEC;
    assert(!cow_file_leaf_refaultable(&vma,&metadata,0,1,clean));
    assert(!cow_file_leaf_refaultable(&vma,&metadata,0,0,clean|PTE_W));
    for(unsigned flags=VM_SHARED;flags<=VM_PFNMAP;flags*=2) {
        if(flags==VM_ANON) continue;
        vma.vm_flags=VM_FILE|flags;
        assert(!cow_file_leaf_refaultable(&vma,&metadata,0,0,clean));
    }
    vma.vm_flags=VM_FILE|VM_VMO;
    assert(!cow_file_leaf_refaultable(&vma,&metadata,0,0,clean));
    vma.vm_flags=VM_FILE;private_copy=1;
    assert(!cow_file_leaf_refaultable(&vma,&metadata,0,0,clean));private_copy=0;
    for(unsigned safe=1;safe<=MM_SAFE_MASK;safe++) {
        metadata.safe[0]=safe;
        assert(!cow_file_leaf_refaultable(&vma,&metadata,0,0,clean));
    }
    metadata.safe[0]=0;cache_pfn=10;
    assert(!cow_file_leaf_refaultable(&vma,&metadata,0,0,clean));cache_pfn=9;
    assert(a20_cow_share_snapshot_at_vma(&parent,0,0,&tables[0][0],tables[0],&vma,&snap)==0);
    assert(!frame_gets&&!cache.pin&&locked==-1&&snap.slot==NULL);
    /* A modified private page later mprotected read-only still has to copy. */
    cache_owners[0]=NULL;
    assert(a20_cow_share_snapshot_at_vma(&parent,0,0,&tables[0][0],tables[0],&vma,&snap)==1);
    assert(frame_gets==1&&snap.kind==1);
    a20_cow_share_unlock(&snap);a20_cow_release_ref(&snap);cache_owners[0]=&cache;

    struct a20_cow_anon_batch_page batch[16];
    int prior_gets=frame_gets,prior_changes=changes;
    assert(a20_cow_share_anon_batch_snapshot(&parent,&parent,0,tables[0],batch,16,0)==1);
    for(int i=0;i<16;i++) {
        assert(!batch[i].class&&!batch[i].kind&&!batch[i].pcp&&!batch[i].pfn&&!batch[i].flags);
        assert(tables[0][i]==clean); /* no COW rewrite/status change for omissions */
    }
    assert(frame_gets==prior_gets&&changes==prior_changes&&!cache.pin&&locked==-1);
    tables[0][4]=0;cache_owners[7]=NULL;expected_va=7*PAGE_SIZE;
    assert(a20_cow_share_anon_batch_snapshot(&parent,&parent,0,tables[0],batch,16,0)==1);
    assert(frame_gets==prior_gets+1&&batch[7].kind==1&&batch[7].class==MM_ST_FILE_PRIVATE);
    for(int i=0;i<16;i++) if(i!=7) assert(!batch[i].class&&!batch[i].kind);
    frame_put(batch[7].pfn);
    puts("COW_SNAPSHOT_TOKEN: PASS tokens, refs, geometry, cache-only refault, safe bits and mixed batches");
    return 0;
}
