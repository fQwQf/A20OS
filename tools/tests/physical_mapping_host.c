/* Real Rust mapping policy, C bridges/consumers and PT unmap; modeled hardware. */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define ARCH_PT_ROOT_LEVEL 2
#define USER_VA_LIMIT 0x40000UL
#define MMAP_BASE_ADDR 0x1000UL
#define PTE_V 1UL
#define PTE_LEAF 2UL
#define PTE_R 4UL
#define PTE_W 8UL
#define PTE_U 16UL
#define PTE_A 32UL
#define PTE_D 64UL
#define PTE_X 128UL
#define PTE_MAT1 256UL
#define PTE_DEVICE 512UL
#define PTE_G 1024UL
#define PTE_COW 2048UL
#define VM_READ 1UL
#define VM_WRITE 2UL
#define VM_EXEC 4UL
#define VM_SHARED 8UL
#define VM_DONTFORK (1UL<<9)
#define VM_PFNMAP (1UL<<17)
#define VM_NATIVE_CAP (1UL<<19)
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_FIXED_NOREPLACE 0x100000
#define ROUND_UP(v,a) (((v)+(a)-1)&~((a)-1))
typedef uintptr_t pte_t;
typedef uintptr_t vaddr_t;
typedef uintptr_t paddr_t;
typedef struct mm_seg {
 uintptr_t start,end;uint64_t vm_flags;uintptr_t pte_flags;uint32_t vmar_cap;
 void *file;uint64_t backing_offset;void *file_vnode,*vmo,*vmar_owner;struct mm_seg *next;
} mm_seg_t;
typedef struct {pte_t *pgdir;int lock;uintptr_t mmap_base;mm_seg_t *mmap;size_t total_vm,rss;} mm_struct_t;
static int live_tables,live_vmas,transaction,changes,retired,fail_after=-1,allocations;
static int record_oom,unmap_error,nommu,installs,unmaps,finishes,cache_error;
static pte_t *retire_list[32];
static int arch_pt_vpn(uintptr_t va,int level){return (va>>(12+2*level))&3;}
static int arch_pt_level_entries(int level){(void)level;return 4;}
static int arch_pte_is_leaf(pte_t p){return p&PTE_LEAF;}
static pte_t *arch_pte_to_ptr(pte_t p){return (void *)(p&~4095UL);}
static void mm_pt_node_lock(pte_t *p){(void)p;assert(transaction);}
static void mm_pt_node_unlock(pte_t *p){(void)p;}
static void mm_pt_note_absent(pte_t *p,int l,int i){(void)p;(void)l;(void)i;}
static void mm_pt_node_clear_seg(pte_t *p,int l,int i){(void)p;(void)l;(void)i;}
static void mm_pt_retire_table(mm_struct_t *mm,pte_t *p,int level){(void)level;assert(mm->lock&&transaction);retire_list[retired++]=p;}
static pte_t *new_table(void){
 if(fail_after>=0&&allocations++==fail_after)return NULL;
 pte_t *p=aligned_alloc(PAGE_SIZE,PAGE_SIZE);assert(p);memset(p,0,PAGE_SIZE);live_tables++;return p;
}
static int pt_map(pte_t *root,uintptr_t va,uintptr_t pa,pte_t flags){
 assert(transaction);installs++;pte_t *table=root;
 for(int l=2;l>0;l--){int i=arch_pt_vpn(va,l);if(!(table[i]&PTE_V)){pte_t *p=new_table();if(!p)return -ENOMEM;table[i]=(uintptr_t)p|PTE_V;}assert(!arch_pte_is_leaf(table[i]));table=arch_pte_to_ptr(table[i]);}
 assert(!table[arch_pt_vpn(va,0)]);table[arch_pt_vpn(va,0)]=pa|flags;return 0;
}
static pte_t *pt_lookup_leaf(pte_t *root,uintptr_t va,int *level,uintptr_t *base,size_t *bytes){
 (void)level;(void)base;(void)bytes;pte_t *table=root;
 for(int l=2;l>0;l--){pte_t *p=&table[arch_pt_vpn(va,l)];if(!(*p&PTE_V)||arch_pte_is_leaf(*p))return p;table=arch_pte_to_ptr(*p);}return &table[arch_pt_vpn(va,0)];
}
static pte_t leaf(mm_struct_t *mm,uintptr_t va){return *pt_lookup_leaf(mm->pgdir,va,NULL,NULL,NULL);}
static void mm_tlb_invalidate_begin(mm_struct_t *mm){assert(!mm->lock&&!transaction);transaction=1;}
static uint64_t spin_lock_irqsave(int *lock){assert(transaction&&!*lock);*lock=1;return 7;}
static void spin_unlock_irqrestore(int *lock,uint64_t f){assert(f==7&&*lock);*lock=0;}
static void mm_tlb_note_change(mm_struct_t *mm,uintptr_t va,size_t n){(void)va;(void)n;assert(mm->lock&&transaction);changes++;}
static void mm_tlb_invalidate_finish(mm_struct_t *mm){assert(!mm->lock&&transaction);for(int i=0;i<retired;i++){free(retire_list[i]);live_tables--;}retired=0;transaction=0;finishes++;}
static void vma_put(mm_struct_t *mm,mm_seg_t *s){assert(!mm->lock&&!transaction);free(s);live_vmas--;}
static void mm_vma_flush_deferred(mm_struct_t *mm){assert(!mm->lock&&!transaction);}
mm_seg_t *mm_seg_new(void){assert(!transaction);if(record_oom)return NULL;live_vmas++;return calloc(1,sizeof(mm_seg_t));}
uintptr_t a20_mmap_user_va_limit(void){return USER_VA_LIMIT;}
uintptr_t a20_mmap_base_addr(void){return MMAP_BASE_ADDR;}
uintptr_t a20_mmap_mmap_base(const void *p){return ((const mm_struct_t *)p)->mmap_base;}
int a20_mmap_is_nommu(void){return nommu;}
int mm_range_overlaps(void *p,uintptr_t start,size_t len,const void *ignore){mm_struct_t *mm=p;assert(mm->lock);for(mm_seg_t *s=mm->mmap;s;s=s->next)if(s!=ignore&&start<s->end&&s->start<start+len)return 1;return 0;}
uintptr_t mm_find_gap(void *p,uintptr_t hint,size_t len){mm_struct_t *mm=p;assert(mm->lock);for(uintptr_t va=hint;va<=USER_VA_LIMIT-len;va+=PAGE_SIZE)if(!mm_range_overlaps(mm,va,len,NULL))return va;return 0;}
void mm_insert_vma(void *p,void *v){mm_struct_t *mm=p;mm_seg_t *s=v;assert(mm->lock&&!mm_range_overlaps(mm,s->start,s->end-s->start,NULL));s->next=mm->mmap;mm->mmap=s;}
void a20_mmap_total_vm_add(void *p,size_t n){mm_struct_t *mm=p;assert(mm->lock);mm->total_vm+=n;}
static void mm_rss_add(mm_struct_t *mm,size_t n){assert(mm->lock);mm->rss+=n;}
static int mm_addr_is_error(vaddr_t a){return a>=(vaddr_t)-4095;}
static int mm_addr_error(vaddr_t a){return -(int)(vaddr_t)(0-a);}
/* Unreachable NOMMU branch of the shared placement helper. */
void *a20_mmap_nommu_alloc(size_t n,uintptr_t *p){(void)n;(void)p;assert(0);return NULL;}
/* PRODUCTION_PT */
int mm_munmap_locked(void *p,uintptr_t addr,size_t len){
 mm_struct_t *mm=p;assert(mm->lock&&transaction);unmaps++;if(unmap_error)return unmap_error;
 mm_seg_t **slot=&mm->mmap;while(*slot){mm_seg_t *s=*slot;if(s->start<addr+len&&addr<s->end){assert(addr<=s->start&&addr+len>=s->end);for(uintptr_t va=s->start;va<s->end;va+=PAGE_SIZE)assert(!pt_unmap(mm,va));size_t pages=(s->end-s->start)/PAGE_SIZE;mm->rss-=pages;mm->total_vm-=pages;*slot=s->next;free(s);live_vmas--;mm_tlb_note_change(mm,addr,len);}else slot=&s->next;}return 0;
}
/* PRODUCTION_MM */
static int udriver_mmio_allowed(uint64_t pa,uint64_t n){return pa>=0x100000&&n<=0x200000-pa&&pa<=0x200000;}
/* PRODUCTION_UDRIVER */
struct device {int unused;};
static uint64_t fb_phys=0x100000;static size_t fb_size=3*PAGE_SIZE;
static int get_fb(struct device *d,uintptr_t *p,size_t *n){(void)d;*p=fb_phys;*n=fb_size;return 0;}
struct gpu_scanout {struct vmo *memory;uint64_t offset;size_t size;};
typedef struct {int (*get_fb)(struct device *,uintptr_t *,size_t *);int (*get_scanout)(struct device *,struct gpu_scanout *);} gpu_dev_ops_t;
struct gpu_device_access {struct device *device;const gpu_dev_ops_t *ops;};
static int gpu_device_scanout_get(const struct gpu_device_access *a,struct gpu_scanout *out){(void)a;(void)out;assert(0);return -EOPNOTSUPP;}
static void gpu_scanout_put(struct gpu_scanout *s){(void)s;assert(0);}
static vaddr_t mm_mmap_vmo_cap(mm_struct_t *mm,vaddr_t addr,size_t len,int prot,int flags,struct vmo *vmo,uint64_t off,uint32_t cap){(void)mm;(void)addr;(void)len;(void)prot;(void)flags;(void)vmo;(void)off;(void)cap;assert(0);return (vaddr_t)-EINVAL;}
typedef struct {mm_struct_t *mm;} task_t;static task_t task;
static task_t *proc_current(void){return &task;}
static void fb_autoflush_kick(void){}
static int boot_framebuffer_user_flags(uint64_t pa,size_t n,uintptr_t requested,uintptr_t *out){(void)pa;assert(n&&!(n&(PAGE_SIZE-1)));if(cache_error)return cache_error;*out=(requested&~(PTE_MAT1|PTE_DEVICE))|PTE_DEVICE;return 0;}
#define FBIO_MAP_FB 1
/* PRODUCTION_FB */
static mm_struct_t fresh(void){assert(!live_tables&&!live_vmas&&!transaction);fail_after=-1;allocations=changes=installs=unmaps=finishes=0;record_oom=unmap_error=nommu=cache_error=0;return (mm_struct_t){.pgdir=new_table()};}
static void cleanup(mm_struct_t *mm){fail_after=-1;uint64_t f=a20_mmap_physical_enter(mm);for(uintptr_t va=0;va<USER_VA_LIMIT;va+=PAGE_SIZE)(void)pt_unmap(mm,va);for(mm_seg_t *s=mm->mmap;s;){mm_seg_t *next=s->next;free(s);live_vmas--;s=next;}mm->mmap=NULL;a20_mmap_physical_finish(mm,f,NULL);free(mm->pgdir);live_tables--;assert(!live_tables&&!live_vmas&&!transaction&&!mm->lock);}
static uintptr_t map(mm_struct_t *mm,uintptr_t addr,size_t n,int prot,int flags,uint64_t pa){return mm_mmap_physical(mm,addr,n,prot,flags,pa,PTE_DEVICE);}
int main(int argc,char **argv){
 assert(argc==2);const char *name=argv[1];mm_struct_t mm=fresh();
 if(!strcmp(name,"arguments")){
  assert((intptr_t)map(NULL,0,PAGE_SIZE,3,1,0)==-EINVAL);
  assert((intptr_t)map(&mm,0,0,3,1,0)==-EINVAL);
  assert((intptr_t)map(&mm,0,SIZE_MAX,3,1,0)==-EINVAL);
  assert((intptr_t)map(&mm,0,PAGE_SIZE,3,1,1)==-EINVAL);
  assert((intptr_t)map(&mm,0,2*PAGE_SIZE,3,1,UINT64_MAX&~4095ULL)==-EINVAL);
  assert((intptr_t)map(&mm,0,PAGE_SIZE,0,1,0)==-EINVAL);
  assert((intptr_t)map(&mm,0,PAGE_SIZE,7,1,0)==-EACCES);
  assert((intptr_t)map(&mm,0,PAGE_SIZE,8,1,0)==-EINVAL);
  assert((intptr_t)map(&mm,0,PAGE_SIZE,3,MAP_PRIVATE,0)==-EINVAL);
  assert((intptr_t)map(&mm,0,PAGE_SIZE,3,1|0x20,0)==-EINVAL);
  assert((intptr_t)mm_mmap_physical(&mm,0,PAGE_SIZE,3,1,0,PTE_X)==-EINVAL);
  assert((intptr_t)map(&mm,3,PAGE_SIZE,3,1|MAP_FIXED,0)==-EINVAL);
  assert((intptr_t)map(&mm,0,PAGE_SIZE,3,1|MAP_FIXED,0)==-EINVAL);
  assert((intptr_t)map(&mm,USER_VA_LIMIT-PAGE_SIZE,2*PAGE_SIZE,3,1|MAP_FIXED,0)==-EINVAL);
  assert(!installs&&!live_vmas&&!transaction&&!finishes);nommu=1;assert((intptr_t)map(&mm,0,PAGE_SIZE,3,1,0)==-EOPNOTSUPP);nommu=0;
 }else if(!strcmp(name,"address-width")){
  uintptr_t va=map(&mm,0,PAGE_SIZE,3,1,0x100000000ULL);
  if(sizeof(paddr_t)==4)assert((intptr_t)va==-EINVAL&&!installs&&!finishes&&!live_vmas);
  else assert(!mm_addr_is_error(va)&&(leaf(&mm,va)&~4095UL)==0x100000000ULL);
 }else if(!strcmp(name,"layout")){
  uintptr_t va=map(&mm,0,PAGE_SIZE+1,PROT_WRITE,1,0);assert(va==MMAP_BASE_ADDR&&mm.rss==2&&mm.total_vm==2&&changes==1);
  mm_seg_t *s=mm.mmap;assert(s->end==va+2*PAGE_SIZE&&s->backing_offset==0&&s->vmar_cap==3);
  assert(s->vm_flags==(VM_READ|VM_WRITE|VM_SHARED|VM_PFNMAP|VM_DONTFORK|VM_NATIVE_CAP));
  assert(leaf(&mm,va)&PTE_DEVICE);assert(!(leaf(&mm,va)&(PTE_X|PTE_MAT1)));assert(!mm.lock&&!transaction);
  uintptr_t next=mm_mmap_physical(&mm,0,PAGE_SIZE,1,1,0x200000,PTE_MAT1);assert(next==va+2*PAGE_SIZE);assert(leaf(&mm,next)&PTE_MAT1);assert(!(leaf(&mm,next)&PTE_DEVICE));
 }else if(!strcmp(name,"protection")){
  pte_t old=PTE_V|PTE_R|PTE_W|PTE_U|PTE_A|PTE_D|PTE_LEAF|PTE_DEVICE;
  pte_t ro=mm_pte_flags_apply_prot(old,mm_prot_to_pte_flags(PROT_READ));
  assert((ro&PTE_DEVICE)==PTE_DEVICE&&(ro&PTE_D)&&!(ro&(PTE_W|PTE_MAT1|PTE_X)));
  pte_t rw=mm_pte_flags_apply_prot(ro,mm_prot_to_pte_flags(PROT_READ|PROT_WRITE));
  assert((rw&PTE_DEVICE)==PTE_DEVICE&&(rw&PTE_W)&&!(rw&PTE_MAT1));
  pte_t normal=mm_pte_flags_apply_prot((old&~PTE_DEVICE)|PTE_MAT1,mm_prot_to_pte_flags(PROT_READ));
  assert((normal&PTE_MAT1)&&!(normal&PTE_DEVICE));
 }else if(!strcmp(name,"placement")){
  uintptr_t va=map(&mm,0,2*PAGE_SIZE,3,1,0x100000);assert(!mm_addr_is_error(va));pte_t saved=leaf(&mm,va);
  assert((intptr_t)map(&mm,va,PAGE_SIZE,3,1|MAP_FIXED_NOREPLACE,0)==-EEXIST&&leaf(&mm,va)==saved&&!unmaps);
  assert(map(&mm,va+PAGE_SIZE,2*PAGE_SIZE,3,1,0)==va+2*PAGE_SIZE);
  assert(map(&mm,USER_VA_LIMIT+PAGE_SIZE,PAGE_SIZE,3,1,0)==va+4*PAGE_SIZE);
  assert(map(&mm,0x8123,PAGE_SIZE,3,1,0)==0x8000);
 }else if(!strcmp(name,"fixed")){
  uintptr_t va=map(&mm,0,2*PAGE_SIZE,3,1,0x100000);unmap_error=-EPERM;
  assert((intptr_t)map(&mm,va,2*PAGE_SIZE,3,1|MAP_FIXED,0)==-EPERM&&mm.rss==2);unmap_error=0;
  assert(map(&mm,va,2*PAGE_SIZE,1,1|MAP_FIXED,0)==va&&mm.rss==2&&mm.total_vm==2&&unmaps==2);
  assert((leaf(&mm,va)&~4095UL)==0&&!(leaf(&mm,va)&PTE_W));
  fail_after=0;allocations=0;
  assert((intptr_t)map(&mm,va,2*PAGE_SIZE,3,1|MAP_FIXED,0x100000)==-ENOMEM);
  assert(!mm.mmap&&!mm.rss&&!mm.total_vm&&live_tables==1&&!leaf(&mm,va));
 }else if(!strcmp(name,"orphan")){
  uint64_t f=a20_mmap_physical_enter(&mm);assert(!pt_map(mm.pgdir,0x2000,0,PTE_V|PTE_LEAF|PTE_R));a20_mmap_physical_finish(&mm,f,NULL);installs=0;
  assert((intptr_t)map(&mm,0x1000,2*PAGE_SIZE,3,1|MAP_FIXED_NOREPLACE,0x100000)==-EEXIST&&!installs&&!mm.mmap&&leaf(&mm,0x2000));
 }else if(!strcmp(name,"oom")){
  record_oom=1;assert((intptr_t)map(&mm,0,PAGE_SIZE,3,1,0)==-ENOMEM&&!finishes&&!installs);record_oom=0;
 }else if(!strcmp(name,"rollback")){
  cleanup(&mm);
  for(int boundary=4;boundary<=16;boundary*=4)for(int fail=0;fail<4;fail++){
   mm=fresh();uint64_t f=a20_mmap_physical_enter(&mm);assert(!pt_map(mm.pgdir,0,0x300000,PTE_V|PTE_LEAF|PTE_R));a20_mmap_physical_finish(&mm,f,NULL);
   int baseline=live_tables;pte_t saved=leaf(&mm,0);fail_after=fail;allocations=0;
   uintptr_t va=(boundary-1)*PAGE_SIZE;uintptr_t ret=map(&mm,va,3*PAGE_SIZE,3,1|MAP_FIXED_NOREPLACE,0x100000);
   if(mm_addr_is_error(ret)){assert((intptr_t)ret==-ENOMEM&&!live_vmas&&!mm.mmap&&live_tables==baseline&&!mm.rss&&!mm.total_vm);for(int i=0;i<3;i++)assert(!leaf(&mm,va+i*PAGE_SIZE));}
   else assert(ret==va&&mm.rss==3&&mm.total_vm==3);
   assert(leaf(&mm,0)==saved);cleanup(&mm);
  }return 0;
 }else if(!strcmp(name,"consumers")){
  uint64_t out=0xfeed;assert(udriver_map_mmio(&mm,0x100001,PAGE_SIZE,3,&out)==-1&&out==0xfeed);
  record_oom=1;assert(udriver_map_mmio(&mm,0x100000,PAGE_SIZE,3,&out)==-1&&out==0xfeed);record_oom=0;
  assert(!udriver_map_mmio(&mm,0x100000,PAGE_SIZE,3,&out)&&mm.rss==1&&(leaf(&mm,out)&PTE_DEVICE));
  struct device dev={0};const gpu_dev_ops_t ops={.get_fb=get_fb};struct gpu_device_access access={&dev,&ops};task.mm=&mm;
  fb_phys=UINTPTR_MAX-(PAGE_SIZE-1);
  assert(fbdev_linux_mmap_active(&access,0,PAGE_SIZE,3,MAP_SHARED,2*PAGE_SIZE)==-EOVERFLOW);
  fb_phys=0x100000;
  uintptr_t va=fbdev_linux_mmap_active(&access,0,3*PAGE_SIZE,3,MAP_SHARED,0);assert(!mm_addr_is_error(va)&&mm.rss==4);
  assert((intptr_t)fbdev_linux_mmap_active(&access,va,3*PAGE_SIZE,3,MAP_SHARED|MAP_FIXED_NOREPLACE,0)==-EEXIST);
  assert(fbdev_linux_mmap_active(&access,va,3*PAGE_SIZE,3,MAP_SHARED|MAP_FIXED,0)==(intptr_t)va&&mm.rss==4);
  assert(!fb_ioctl_map(&access,(void *)0x10000)&&mm.rss==7);
  assert(fb_ioctl_map(&access,(void *)0x10000)==-EEXIST);
  cache_error=-EOPNOTSUPP;assert(fbdev_linux_mmap_active(&access,0,PAGE_SIZE,3,MAP_SHARED,0)==-EOPNOTSUPP);assert(fb_ioctl_map(&access,(void *)0x20000)==-EOPNOTSUPP);cache_error=0;
 }else assert(0);
 cleanup(&mm);return 0;
}
