/* Production DRM, native device/class lifetimes and GPU selection. VFS, VMO,
 * PTE mapping, work scheduling and Park/Wake are explicit host boundaries. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#undef RENAME_NOREPLACE
#undef RENAME_EXCHANGE
#undef RENAME_WHITEOUT
#include "drivers/gpu/gpu_core.c"
#include "drivers/gpu/drm.c"
unsigned host_cpu;
uintptr_t host_page_offset;
static _Thread_local task_t current_task;
struct task_t *proc_current(void){return &current_task;}
void proc_yield(void){sched_yield();}
static long heap_live;
static int fail_after=-1;
void *kmalloc(size_t n){if(fail_after==0)return NULL;if(fail_after>0)fail_after--;void *p=malloc(n?n:1);assert(p);__atomic_add_fetch(&heap_live,1,__ATOMIC_RELAXED);return p;}
void *kcalloc(size_t n,size_t z){void *p=kmalloc(n*z);if(p)memset(p,0,n*z);return p;}
void *krealloc(void *p,size_t n){if(!p)return kmalloc(n);if(fail_after==0)return NULL;return realloc(p,n);}
void kfree(void *p){if(p){__atomic_sub_fetch(&heap_live,1,__ATOMIC_RELAXED);free(p);}}
void panic(const char *fmt,...){(void)fmt;abort();}
void host_log(const char *fmt,...){(void)fmt;}
void netlink_uevent_emit(const char*a,const char*b,const char*c,uint64_t d){(void)a;(void)b;(void)c;(void)d;}
int udriver_mmio_user_owned(uint64_t p){(void)p;return 0;}
int driver_memory_conflict(uint64_t a,uint64_t b){(void)a;(void)b;return 0;}
int driver_memory_intersects(uint64_t a,uint64_t b){(void)a;(void)b;return 0;}
void *arch_device_map_mmio(uint64_t p,size_t n,uint32_t f){(void)p;(void)n;(void)f;return NULL;}
void arch_device_unmap_mmio(void *p,size_t n){(void)p;(void)n;}
__asm__(".global __driver_init_start\n.global __driver_init_end\n__driver_init_start:\n__driver_init_end:\n");
/* Actual mutex shapes, pthread implementations. No architecture lock/IRQ code
 * runs on the host; recursive ownership matches the native mutex contract. */
struct mutex_back {mutex_t *key;pthread_mutex_t native;struct mutex_back *next;};
static struct mutex_back *mutexes;
static pthread_mutex_t mutex_map=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local mutex_t *observed_mutex;
static _Thread_local unsigned observed_depth;
static int close_import_fd;
static struct mutex_back *get_mutex(mutex_t *m){
 assert(!pthread_mutex_lock(&mutex_map));struct mutex_back *b=mutexes;
 while(b&&b->key!=m)b=b->next;
 if(!b){b=calloc(1,sizeof(*b));assert(b);b->key=m;pthread_mutexattr_t a;assert(!pthread_mutexattr_init(&a));assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));assert(!pthread_mutex_init(&b->native,&a));assert(!pthread_mutexattr_destroy(&a));b->next=mutexes;mutexes=b;}
 assert(!pthread_mutex_unlock(&mutex_map));return b;
}
void mutex_init(mutex_t *m){(void)get_mutex(m);}
void mutex_lock(mutex_t *m){assert(!pthread_mutex_lock(&get_mutex(m)->native));if(m==observed_mutex)observed_depth++;}
void mutex_unlock(mutex_t *m){if(m==observed_mutex){assert(observed_depth);observed_depth--;}assert(!pthread_mutex_unlock(&get_mutex(m)->native));}
int mutex_trylock(mutex_t *m){return !pthread_mutex_trylock(&get_mutex(m)->native);}
void wait_queue_init(wait_queue_t *q){memset(q,0,sizeof(*q));spin_init(&q->lock);}
unsigned wait_queue_wake_all(wait_queue_t *q,uintptr_t key,proc_wake_reason_t r){(void)q;(void)key;(void)r;return 0;}
static void (*on_link)(void);
bool wait_queue_link(wait_queue_t *q,wait_queue_entry_t *e,proc_wait_token_t t,uintptr_t key){(void)q;(void)e;(void)t;(void)key;if(on_link)on_link();return true;}
void wait_queue_unlink(wait_queue_t *q,wait_queue_entry_t *e){(void)q;(void)e;}
proc_wait_token_t proc_park_prepare(proc_wait_mode_t m,uint64_t d){(void)m;(void)d;return (proc_wait_token_t){.task=proc_current(),.seq=1};}
int proc_park_cancel(proc_wait_token_t t){(void)t;return 0;}
proc_wake_reason_t proc_park_commit(proc_wait_token_t t){(void)t;assert(0);return PROC_WAKE_EVENT;}
void proc_park_finish(proc_wait_token_t t){(void)t;}
static uint64_t ticks=10;
uint64_t clock_get_ticks(void){return ticks++;}
uint64_t clock_ticks_per_sec(void){return 1000;}
void timekeeping_get_monotonic(uint64_t t[2]){t[0]=1;t[1]=2;}
static void (*on_copy)(void);
static int copied_fault,copyout_fault;
int copy_from_user(void *to,const void *from,size_t n){if(!to||!from||copied_fault)return -1;memcpy(to,from,n);if(on_copy)on_copy();return 0;}
int copy_to_user(void *to,const void *from,size_t n){if(!to||!from||copyout_fault)return -1;memcpy(to,from,n);return 0;}
void fbcon_suspend(void){}
void fbcon_resume(void){}
int sync_file_wait_completed(int fd){(void)fd;return 0;}
int sync_file_create_completed(const char *n){(void)n;return -EOPNOTSUPP;}
struct driver_workqueue {int alive;};
struct driver_work {void (*fn)(void *);void *context;int pending;};
struct driver_workqueue *driver_workqueue_create(void){struct driver_workqueue *q=kmalloc(sizeof(*q));if(q)q->alive=1;return q;}
struct driver_work *driver_work_create(struct driver_workqueue *q,void (*fn)(void *),void *context){assert(q&&q->alive);struct driver_work *w=kmalloc(sizeof(*w));if(w)*w=(struct driver_work){fn,context,0};return w;}
int driver_workqueue_destroy(struct driver_workqueue **q,uint64_t d){(void)d;kfree(*q);*q=NULL;return 0;}
int driver_work_schedule(struct driver_work *w,uint64_t d){(void)d;assert(w);int was=w->pending;w->pending=1;return !was;}
int driver_work_reschedule(struct driver_work *w,uint64_t d){return driver_work_schedule(w,d);}
pfa_t pfa;
#define HOST_PAGES 4096
static unsigned char page_used[HOST_PAGES];
static unsigned vmo_live, page_live;
static unsigned char *host_ram;
struct vmo *vmo_create(uint32_t type,uint64_t size,uint32_t options){struct vmo *v=kcalloc(1,sizeof(*v));if(!v)return NULL;vmo_live++;v->type=type;v->size=size;v->options=options;refcount_set(&v->refcount,1);v->page_count=(size+PAGE_SIZE-1)/PAGE_SIZE;v->pages=kmalloc(v->page_count*sizeof(pfn_t));if(!v->pages){vmo_live--;kfree(v);return NULL;}for(uint32_t i=0;i<v->page_count;i++)v->pages[i]=PFN_NONE;return v;}
void vmo_ref(struct vmo *v){if(v)refcount_inc(&v->refcount);}
static int device_references_page(pfn_t);
void vmo_release(struct vmo *v){
 if(!v||!refcount_dec_and_test(&v->refcount))return;
 for(uint32_t i=0;i<v->page_count;i++)if(v->pages[i]!=PFN_NONE){assert(page_used[v->pages[i]]==1&&!device_references_page(v->pages[i]));page_used[v->pages[i]]=0;page_live--;}
 kfree(v->pages);vmo_live--;kfree(v);
}
pfn_t vmo_peek_page(struct vmo *v,uint32_t i){return i<v->page_count?v->pages[i]:PFN_NONE;}
int vmo_get_page_charged(struct vmo *v,uint32_t i,struct cg_node *cg,pfn_t *out){
 (void)cg;if(i>=v->page_count)return -EINVAL;
 if(v->pages[i]==PFN_NONE){for(pfn_t p=0;p<HOST_PAGES;p++)if(!page_used[p]){page_used[p]=1;page_live++;v->pages[i]=p;memset(host_ram+p*PAGE_SIZE,0,PAGE_SIZE);break;}}
 if(v->pages[i]==PFN_NONE)return -ENOMEM;*out=v->pages[i];return 0;
}
int vmo_contiguous_info(const struct vmo *v,uint64_t *p,size_t *n){(void)v;(void)p;(void)n;return -EINVAL;}
static struct {struct vmo *memory;int prot,flags;uint32_t cap;size_t size;uint64_t offset;} maps[32];
static unsigned map_count;static int map_error;
pte_t mm_prot_to_pte_flags(int prot){return prot|((prot&PROT_WRITE)?PROT_READ:0);}
int mm_pte_flags_to_prot(pte_t flags){return (int)flags;}
vaddr_t mm_mmap_vmo_cap(mm_struct_t *mm,vaddr_t addr,size_t n,int prot,int flags,struct vmo *v,uint64_t off,uint32_t cap){assert(mm&&map_count<32);if(map_error)return (vaddr_t)map_error;vmo_ref(v);maps[map_count].memory=v;maps[map_count].prot=prot;maps[map_count].flags=flags;maps[map_count].cap=cap;maps[map_count].size=n;maps[map_count].offset=off;map_count++;return addr?addr:0x10000+map_count*0x10000;}
static void unmap_all(void){while(map_count)vmo_release(maps[--map_count].memory);}
static uint64_t identities;
struct vfile *vfile_alloc(void){vfile_t *v=kcalloc(1,sizeof(*v));if(v)v->identity=++identities;return v;}
void vfile_free(struct vfile *v){kfree(v);}
void vfile_ref_init(struct vfile *v,int n){refcount_set(&v->ref_count,n);}
void vfile_get(struct vfile *v){refcount_inc(&v->ref_count);}
void vnode_ref_init(vnode_t *v,int n){refcount_set(&v->ref_count,n);}
void vnode_get(vnode_t *v){refcount_inc(&v->ref_count);}
void vnode_put(vnode_t *v){if(v&&refcount_dec_and_test(&v->ref_count)){if(v->ops&&v->ops->release)v->ops->release(v);else kfree(v);}}
void vfs_put_file(struct vfile *v){assert(!observed_depth);if(v&&refcount_dec_and_test(&v->ref_count)){if(v->ops&&v->ops->close)v->ops->close(v);vnode_put(v->vnode);vfile_free(v);}}
static vfile_t *fds[64];
static int fd_install_error;
int fdtable_install_current_vfile(vfile_t *v,int flags){assert(!observed_depth);(void)flags;if(fd_install_error)return fd_install_error;for(int i=3;i<64;i++)if(!fds[i]){fds[i]=v;return i;}return -EMFILE;}
/* PRODUCTION_ANONFD */
struct vfile *fdtable_get_current_file_ref(int n){assert(!observed_depth);if(n<0||n>=64||!fds[n])return NULL;vfile_t *v=fds[n];vfile_get(v);if(close_import_fd){close_import_fd=0;fds[n]=NULL;vfs_put_file(v);}return v;}
int fdtable_close_current(int n){assert(!observed_depth);if(n<0||n>=64||!fds[n])return -EBADF;vfile_t *v=fds[n];fds[n]=NULL;vfs_put_file(v);return 0;}
int firmware_device_prepare(device_t *d){(void)d;return 0;}
const boot_context_t *boot_context_get(void){static boot_context_t context;return &context;}
void *boot_framebuffer_map(void){return NULL;}
void proc_wake_q_init(proc_wake_q_t *q){q->count=0;}
unsigned proc_wake_q_flush(proc_wake_q_t *q){unsigned n=q->count;q->count=0;return n;}
unsigned wait_queue_collect_all(wait_queue_t *q,uintptr_t key,proc_wake_reason_t why,proc_wake_q_t *wake,bool *complete){(void)wake;if(complete)*complete=true;return wait_queue_wake_all(q,key,why);}
struct sample {
 device_t device;driver_t driver;gpu_dev_ops_t *ops;class_device_t *publication;
 unsigned id,info_calls,edid_calls,unrefs,ctx_destroys,submits,transfers,flushes;
 int remove_error,unref_error,ctx_error,create_error,attach_error,destroy_error;
 int ctx_live[128],resource_live[128];pfn_t backing[128][16];unsigned backing_count[128];int stopped;
};
static struct sample *samples[4];
static struct sample *get_sample(device_t *d){struct sample *s=d->plat_data;assert(s&&d->drv==&s->driver&&s->publication&&s->publication->active_calls&&!s->stopped);return s;}
static pthread_mutex_t pause_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_changed=PTHREAD_COND_INITIALIZER;
static int pause_info,info_entered,info_release;
static int sample_info(device_t *d,uint32_t *w,uint32_t *h,uint32_t *b){
 struct sample *s=get_sample(d);s->info_calls++;
 if(pause_info&&s->id==1){assert(!pthread_mutex_lock(&pause_lock));info_entered=1;assert(!pthread_cond_broadcast(&pause_changed));while(!info_release)assert(!pthread_cond_wait(&pause_changed,&pause_lock));assert(!pthread_mutex_unlock(&pause_lock));}
 *w=16+s->id;*h=8+s->id;*b=32;return 0;
}
static int sample_fb(device_t *d,uintptr_t *p,size_t *n){struct sample *s=get_sample(d);*p=s->id*PAGE_SIZE;*n=PAGE_SIZE;return 0;}
static int sample_flush(device_t *d,uint32_t x,uint32_t y,uint32_t w,uint32_t h){struct sample *s=get_sample(d);assert(x+w<=16+s->id&&y+h<=8+s->id);s->flushes++;return 0;}
static int sample_edid(device_t *d,uint8_t *buf,size_t n){struct sample *s=get_sample(d);assert(n>=128);s->edid_calls++;drm_edid_synthesize(buf,16+s->id,8+s->id);buf[12]=s->id;unsigned sum=0;for(unsigned i=0;i<127;i++)sum+=buf[i];buf[127]=(uint8_t)-sum;return 128;}
static int sample_features(device_t *d,uint32_t *a,uint32_t *b){get_sample(d);if(a)*a=1;if(b)*b=1;return 0;}
static int sample_capset(device_t *d,uint32_t idx,uint32_t *id,uint32_t *ver,uint32_t *size){get_sample(d);if(idx)return -ENODEV;*id=1;*ver=1;*size=4;return 0;}
static int sample_caps(device_t *d,uint32_t cid,uint32_t idx,uint32_t ver,void *buf,size_t n,size_t *out){get_sample(d);(void)cid;(void)idx;(void)ver;if(n<4)return -EINVAL;memset(buf,0,4);*out=4;return 0;}
static int sample_ctx_create(device_t *d,uint32_t id,uint32_t flags,const char *n,size_t len){struct sample *s=get_sample(d);(void)flags;(void)n;(void)len;assert(id&&id<128);s->ctx_live[id]=1;return s->ctx_error;}
static int sample_ctx_destroy(device_t *d,uint32_t id){struct sample *s=get_sample(d);assert(id&&id<128);s->ctx_destroys++;if(s->destroy_error)return s->destroy_error;s->ctx_live[id]=0;return 0;}
static int sample_create(device_t *d,uint32_t ctx,uint32_t id,uint32_t target,uint32_t format,uint32_t bind,uint32_t w,uint32_t h,uint32_t depth,uint32_t array,uint32_t level,uint32_t nr,uint32_t flags){
 struct sample *s=get_sample(d);assert(ctx<128&&s->ctx_live[ctx]&&id&&id<128);(void)target;(void)format;(void)bind;(void)w;(void)h;(void)depth;(void)array;(void)level;(void)nr;(void)flags;s->resource_live[id]=1;return s->create_error;
}
static int sample_unref(device_t *d,uint32_t id){struct sample *s=get_sample(d);assert(id&&id<128);s->unrefs++;if(s->unref_error)return s->unref_error;s->resource_live[id]=0;s->backing_count[id]=0;return 0;}
static int sample_backing(device_t *d,uint32_t id,const struct virtio_gpu_mem_entry *entries,uint32_t n){
 struct sample *s=get_sample(d);assert(id<128&&s->resource_live[id]&&n<=16);for(unsigned i=0;i<n;i++){pfn_t p=phys_to_pfn(entries[i].addr);assert(page_used[p]==1);s->backing[id][i]=p;}s->backing_count[id]=n;return s->attach_error;
}
static int sample_ctx_attach(device_t *d,uint32_t ctx,uint32_t id){struct sample *s=get_sample(d);assert(ctx<128&&s->ctx_live[ctx]&&id<128&&s->resource_live[id]);return 0;}
static int sample_submit(device_t *d,uint32_t ctx,const void *buf,size_t n){struct sample *s=get_sample(d);assert(ctx<128&&s->ctx_live[ctx]&&buf&&n);s->submits++;return 0;}
static int sample_transfer(device_t *d,uint32_t ctx,uint32_t id,const struct virtio_gpu_box *box,uint32_t l,uint32_t stride,uint32_t layer,uint64_t off,int upload){struct sample *s=get_sample(d);(void)ctx;(void)box;(void)l;(void)stride;(void)layer;(void)off;(void)upload;assert(id<128&&s->resource_live[id]);s->transfers++;return 0;}
static int sample_match(device_t *d,const driver_t *r){return &((struct sample *)d->plat_data)->driver==r;}
static bus_type_t sample_bus={.name="drm-native-test",.match=sample_match};
static int sample_probe(device_t *d){struct sample *s=d->plat_data;memset(s->ctx_live,0,sizeof(s->ctx_live));memset(s->resource_live,0,sizeof(s->resource_live));memset(s->backing_count,0,sizeof(s->backing_count));s->stopped=0;return gpu_device_register(d);}
static int sample_remove(device_t *d){struct sample *s=d->plat_data;assert(!class_device_binding_stopped(s->publication));if(s->remove_error)return s->remove_error;s->stopped=1;gpu_device_unregister(d);return 0;}
static int omit_remove;
static int plain_sample_probe(device_t *d){(void)d;return 0;}
static void create_sample(struct sample *s,unsigned id){
 memset(s,0,sizeof(*s));s->id=id;s->ops=calloc(1,sizeof(*s->ops));assert(s->ops);
 *s->ops=(gpu_dev_ops_t){.get_info=sample_info,.get_fb=sample_fb,.flush=sample_flush,.get_edid=sample_edid,.get_features=sample_features,.get_capset=sample_caps,.capset_info=sample_capset,.resource_attach_backing=sample_backing,.ctx_create=sample_ctx_create,.ctx_destroy=sample_ctx_destroy,.resource_create_3d=sample_create,.resource_unref=sample_unref,.ctx_attach_resource=sample_ctx_attach,.submit_3d=sample_submit,.transfer_3d=sample_transfer};
 s->driver=(driver_t){.name="drm-test",.bus=&sample_bus,.probe=omit_remove?plain_sample_probe:sample_probe,.remove=omit_remove?NULL:sample_remove,.class_ops=s->ops,.class_type=DEV_CLASS_DISPLAY};
 s->device=(device_t){.name="drm-test",.bus=&sample_bus,.plat_data=s,.references=1};
 samples[id]=s;page_used[id]=2;assert(!driver_register(&s->driver)&&!device_register(&s->device)&&s->device.drv==&s->driver);s->publication=class_device_ref_for_device(&s->device);assert(s->publication&&!class_device_binding_stopped(s->publication));
}
static int device_references_page(pfn_t page){
 for(unsigned i=0;i<4;i++){struct sample *s=samples[i];if(!s||s->stopped)continue;for(unsigned id=1;id<128;id++)if(s->resource_live[id])for(unsigned j=0;j<s->backing_count[id];j++)if(s->backing[id][j]==page)return 1;}return 0;
}
static void pump(void){assert(g_drm_reaper_work);g_drm_reaper_work->pending=0;g_drm_reaper_work->fn(g_drm_reaper_work->context);}
static vfile_t *open_sample(struct sample *s){vfile_t *v=NULL;assert(!drm_create_vfile_for(s->publication,&v)&&v);return v;}
static uint32_t dumb(vfile_t *v){struct drm_mode_create_dumb c={.width=32,.height=32,.bpp=32};assert(!v->ops->ioctl(v,DRM_IOCTL_MODE_CREATE_DUMB,&c)&&c.handle&&c.pitch==128&&c.size==PAGE_SIZE);return c.handle;}
static struct drm_virtgpu_resource_create resource(vfile_t *v,int expected){struct drm_virtgpu_resource_create c={.width=4,.height=4,.size=PAGE_SIZE};int result=v->ops->ioctl(v,DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,&c);assert(result==expected);return c;}
static void close_gem(vfile_t *v,uint32_t handle){struct drm_gem_close c={handle,0};assert(!v->ops->ioctl(v,DRM_IOCTL_GEM_CLOSE,&c));}
static int prime(vfile_t *v,uint32_t handle){struct drm_prime_handle p={.handle=handle,.flags=O_RDWR};assert(!v->ops->ioctl(v,DRM_IOCTL_PRIME_HANDLE_TO_FD,&p)&&p.fd>=3);return p.fd;}
static void dispose_sample(struct sample *s){
 s->remove_error=0;int result=driver_unregister(&s->driver);assert(!result||(result==-ENOENT&&!s->device.drv));assert(class_device_binding_stopped(s->publication));
 free(s->ops);s->ops=NULL;assert(!device_unregister(&s->device));class_device_put(s->publication);s->publication=NULL;device_put(&s->device);
}
static void dimensions(vfile_t *v,unsigned id){struct drm_mode_card_res res={0};assert(!v->ops->ioctl(v,DRM_IOCTL_MODE_GETRESOURCES,&res)&&res.max_width==16+id&&res.max_height==8+id);}

static struct sample *hook_sample;
static void withdraw_hook(void){on_link=NULL;assert(!driver_unregister(&hook_sample->driver));}
static void promote_hook(void){on_copy=NULL;gpu_device_unregister(&hook_sample->device);}
static unsigned action_count,action_order[8];
static void action_a(void *p){action_order[action_count++]=*(unsigned *)p;}
static void action_b(void *p){action_order[action_count++]=10+*(unsigned *)p;}
static void *withdraw_thread(void *p){struct sample *s=p;int result=driver_unregister(&s->driver);assert(!result);return NULL;}
struct io_args {vfile_t *file;int result;};
static void *dimensions_thread(void *p){struct io_args *args=p;struct drm_mode_card_res res={0};args->result=args->file->ops->ioctl(args->file,DRM_IOCTL_MODE_GETRESOURCES,&res);return NULL;}
int main(int argc,char **argv){
 assert(argc==2);const char *name=argv[1];driver_core_init();long baseline=heap_live;
 host_ram=aligned_alloc(PAGE_SIZE,HOST_PAGES*PAGE_SIZE);assert(host_ram);host_page_offset=(uintptr_t)host_ram;pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0,.end=HOST_PAGES*PAGE_SIZE,.start_pfn=0,.end_pfn=HOST_PAGES};pfa.total_frames=HOST_PAGES;
 if(!strcmp(name,"actions")){
  device_t device={0};unsigned a=1,b=2;driver_lifecycle_begin();
  assert(device_release_resource_action(NULL,action_a,&a)==-EINVAL&&device_release_resource_action(&device,NULL,&a)==-EINVAL);
  assert(!device_add_resource_action(&device,action_a,&a)&&!device_add_resource_action(&device,action_a,&b)&&!device_add_resource_action(&device,action_b,&a));
  assert(!device_release_resource_action(&device,action_a,&a)&&action_count==1&&action_order[0]==1);
  assert(device_release_resource_action(&device,action_a,&a)==-ENOENT&&action_count==1);
  device_release_resources(&device);assert(action_count==3&&action_order[1]==11&&action_order[2]==2&&!device.resource_actions);driver_lifecycle_end();
 }else{
  struct sample a,b;omit_remove=!strcmp(name,"missing-remove");create_sample(&a,1);vfile_t *fa=NULL,*fa2=NULL,*fb=NULL;
  if(omit_remove){vfile_t *out=(void *)1;assert(drm_create_vfile_for(a.publication,&out)==-EOPNOTSUPP&&!out&&!a.publication->active_calls&&!g_drm_devices);assert(!driver_unregister(&a.driver)&&!class_device_binding_stopped(a.publication));assert(!device_unregister(&a.device));class_device_put(a.publication);device_put(&a.device);free(a.ops);samples[1]=NULL;goto done;}
  if(!strncmp(name,"open-oom-",9)){
   long before=heap_live;int refs=refcount_read(&a.publication->refs);fail_after=name[9]-'0';vfile_t *out=(void *)1;assert(drm_create_vfile_for(a.publication,&out)==-ENOMEM&&!out&&!a.publication->active_calls&&refcount_read(&a.publication->refs)==refs&&!g_drm_devices);fail_after=-1;
   assert(heap_live==before+(g_drm_reaper_work?2:0));fa=open_sample(&a);
  }else{
   fa=open_sample(&a);
   if(!strcmp(name,"binding")||!strcmp(name,"namespaces")||!strcmp(name,"prime")||!strcmp(name,"prime-boundaries")||!strcmp(name,"edid")){
    fa2=open_sample(&a);create_sample(&b,2);fb=open_sample(&b);assert(((drm_context_t *)fa->priv)->device==((drm_context_t *)fa2->priv)->device&&((drm_context_t *)fa->priv)->device!=((drm_context_t *)fb->priv)->device);
    gpu_device_unregister(&a.device);dimensions(fa,1);dimensions(fb,2);
    if(!strcmp(name,"binding")){hook_sample=&b;on_copy=promote_hook;dimensions(fa,1);assert(!on_copy);dimensions(fa2,1);}
    if(!strcmp(name,"namespaces")){uint32_t ha=dumb(fa),hb=dumb(fb);assert(ha==hb);struct drm_mode_map_dumb m={.handle=ha};assert(!fa2->ops->ioctl(fa2,DRM_IOCTL_MODE_MAP_DUMB,&m));close_gem(fa,ha);assert(fa->ops->ioctl(fa,DRM_IOCTL_MODE_MAP_DUMB,&m)==-ENOENT);assert(!fb->ops->ioctl(fb,DRM_IOCTL_MODE_MAP_DUMB,&m));close_gem(fb,hb);}
    if(!strcmp(name,"prime-boundaries")){
     uint32_t h=dumb(fa);int fd=prime(fa,h);struct drm_prime_handle imported={.fd=fd,.handle=99};
     observed_mutex=&((drm_context_t *)fb->priv)->device->gpu_lock;close_import_fd=1;
     assert(fb->ops->ioctl(fb,DRM_IOCTL_PRIME_FD_TO_HANDLE,&imported)==-EXDEV&&imported.handle==99&&!fds[fd]&&!observed_depth);
     close_gem(fa,h);assert(!vmo_live);
     observed_mutex=&((drm_context_t *)fa->priv)->device->gpu_lock;
     h=dumb(fa);fd_install_error=-EMFILE;struct drm_prime_handle failed={.handle=h,.flags=O_RDWR};
     assert(fa->ops->ioctl(fa,DRM_IOCTL_PRIME_HANDLE_TO_FD,&failed)==-EMFILE);fd_install_error=0;
     copyout_fault=1;assert(fa->ops->ioctl(fa,DRM_IOCTL_PRIME_HANDLE_TO_FD,&failed)==-EFAULT);copyout_fault=0;
     fd=prime(fa,h);close_gem(fa,h);imported=(struct drm_prime_handle){.fd=fd};
     copyout_fault=1;assert(fa2->ops->ioctl(fa2,DRM_IOCTL_PRIME_FD_TO_HANDLE,&imported)==-EFAULT);copyout_fault=0;
     assert(!((drm_gem_t *)fds[fd]->priv)->dumb_live&&!fdtable_close_current(fd)&&!vmo_live&&!observed_depth);
     observed_mutex=NULL;
    }
    if(!strcmp(name,"prime")){uint32_t h=dumb(fa);int fd=prime(fa,h);struct drm_prime_handle imported={.fd=fd};assert(!fa2->ops->ioctl(fa2,DRM_IOCTL_PRIME_FD_TO_HANDLE,&imported)&&imported.handle==h);imported.handle=99;assert(fb->ops->ioctl(fb,DRM_IOCTL_PRIME_FD_TO_HANDLE,&imported)==-EXDEV&&imported.handle==99);close_gem(fa,h);assert(!fdtable_close_current(fd));}
    if(!strcmp(name,"edid")){drm_call_t ca,cb;assert(!drm_call_begin(((drm_context_t *)fa->priv)->device,fa->priv,&ca,true));assert(drm_edid_get(&ca)[12]==1);drm_call_end(&ca);assert(!drm_call_begin(((drm_context_t *)fb->priv)->device,fb->priv,&cb,true));assert(drm_edid_get(&cb)[12]==2);drm_call_end(&cb);assert(!drm_call_begin(((drm_context_t *)fa2->priv)->device,fa2->priv,&ca,true));assert(drm_edid_get(&ca)[12]==1&&a.edid_calls==1&&b.edid_calls==1);drm_call_end(&ca);}
    vfs_put_file(fb);fb=NULL;dispose_sample(&b);
   }else if(!strcmp(name,"unref-retry")||!strcmp(name,"offline-retention")||!strcmp(name,"dmabuf-offline")||!strcmp(name,"attach-failure")||!strcmp(name,"resource-failure")){
    if(!strcmp(name,"attach-failure"))a.attach_error=-ETIMEDOUT;
    if(!strcmp(name,"resource-failure"))a.create_error=-ETIMEDOUT;
    if(a.attach_error||a.create_error)a.unref_error=-EIO;
    struct drm_virtgpu_resource_create c=resource(fa,a.attach_error||a.create_error?-ETIMEDOUT:0);
    drm_device_t *device=((drm_context_t *)fa->priv)->device;
    int fd=-1;if(!strcmp(name,"dmabuf-offline"))fd=prime(fa,c.bo_handle);
    if(!(a.attach_error||a.create_error)){a.unref_error=-ETIMEDOUT;close_gem(fa,c.bo_handle);}
    if(!strcmp(name,"unref-retry")){assert(device->retained&&vmo_live==1&&page_live==1);pump();assert(device->retained&&vmo_live==1&&page_live==1);a.unref_error=0;pump();assert(!device->retained&&!vmo_live&&!page_live);}
    else{
     a.remove_error=-ETIMEDOUT;assert(driver_unregister(&a.driver)==-ETIMEDOUT&&!class_device_binding_stopped(a.publication));pump();assert(vmo_live==1&&a.device.drv==&a.driver);
     if(fd>=0){uint64_t sync=1;assert(fds[fd]->ops->ioctl(fds[fd],0x40086200UL,&sync)==-ENODEV);a.remove_error=0;assert(!driver_unregister(&a.driver)&&class_device_binding_stopped(a.publication));pump();assert(vmo_live==1&&!((drm_gem_t *)fds[fd]->priv)->is_virgl);assert(!fdtable_close_current(fd));pump();assert(!vmo_live&&!page_live);}
     else{a.remove_error=0;assert(!driver_unregister(&a.driver));pump();assert(!vmo_live&&!page_live);}
    }
   }else if(!strcmp(name,"context-failure")){
    a.ctx_error=-ETIMEDOUT;a.destroy_error=-EIO;resource(fa,-ETIMEDOUT);drm_context_t *ctx=fa->priv;assert(ctx->virtgpu_ctx_created<0&&ctx->virtgpu_ctx_id);unsigned cid=ctx->virtgpu_ctx_id;assert(resource(fa,-EIO).bo_handle==0);vfs_put_file(fa);fa=NULL;assert(g_drm_devices->retained_contexts&&a.ctx_live[cid]);pump();assert(g_drm_devices->retained_contexts);a.destroy_error=0;pump();assert(!g_drm_devices->retained_contexts&&!a.ctx_live[cid]);
   }else if(!strcmp(name,"mmap")){
    static mm_struct_t mm;current_task.mm=&mm;uint32_t h=dumb(fa);struct drm_mode_map_dumb offset={.handle=h};assert(!fa->ops->ioctl(fa,DRM_IOCTL_MODE_MAP_DUMB,&offset));
    assert(fa->ops->mmap(fa,0,PAGE_SIZE,PROT_READ,MAP_SHARED,offset.offset)>0&&map_count==1&&maps[0].cap==PROT_READ&&maps[0].size==PAGE_SIZE);struct vmo *memory=maps[0].memory;
    pfn_t page;assert(!vmo_get_page_charged(memory,0,NULL,&page));host_ram[page*PAGE_SIZE]=0x67;
    assert(fa->ops->mmap(fa,0,PAGE_SIZE,PROT_READ|PROT_EXEC,MAP_SHARED,offset.offset)==-EACCES);
    assert(fa->ops->mmap(fa,0,PAGE_SIZE,PROT_READ,MAP_PRIVATE,offset.offset)==-EINVAL);
    assert(fa->ops->mmap(fa,0,PAGE_SIZE,PROT_READ,MAP_SHARED,(((uint64_t)1<<32)+h)*PAGE_SIZE)==-EINVAL);
    assert(fa->ops->mmap(fa,0,PAGE_SIZE,PROT_READ,MAP_SHARED,offset.offset+1)==-EINVAL);
    map_error=-EEXIST;assert(fa->ops->mmap(fa,0x10000,PAGE_SIZE,PROT_READ,MAP_SHARED|MAP_FIXED_NOREPLACE,offset.offset)==-EEXIST&&map_count==1);map_error=0;
    close_gem(fa,h);assert(vmo_live==1&&page_live==1&&host_ram[page*PAGE_SIZE]==0x67);unmap_all();assert(!vmo_live&&!page_live);
   }else if(!strcmp(name,"copyout-rollback")){
    copyout_fault=1;struct drm_mode_create_dumb d={.width=32,.height=32,.bpp=32};assert(fa->ops->ioctl(fa,DRM_IOCTL_MODE_CREATE_DUMB,&d)==-EFAULT&&!vmo_live);
    struct drm_gem_create g={.size=PAGE_SIZE};assert(fa->ops->ioctl(fa,A20_GPU_IOCTL_GEM_CREATE,&g)==-EFAULT&&!vmo_live);copyout_fault=0;
   }else if(!strcmp(name,"rebind-resource")){
    struct drm_virtgpu_resource_create old_resource=resource(fa,0);assert(vmo_live==1&&page_live==1);class_device_t *old=a.publication;assert(!driver_unregister(&a.driver)&&class_device_binding_stopped(old));assert(!driver_register(&a.driver));a.publication=class_device_ref_for_device(&a.device);fa2=open_sample(&a);struct drm_virtgpu_resource_create fresh_resource=resource(fa2,0);assert(old_resource.res_handle==fresh_resource.res_handle&&vmo_live==2);unsigned unrefs=a.unrefs,destroys=a.ctx_destroys;pump();assert(vmo_live==1&&page_live==1&&a.unrefs==unrefs&&a.ctx_destroys==destroys&&a.resource_live[fresh_resource.res_handle]);vfs_put_file(fa);fa=NULL;assert(a.ctx_destroys==destroys&&a.ctx_live[1]);class_device_put(old);
   }else if(!strcmp(name,"rebind")){
    uint32_t h=dumb(fa);class_device_t *old=a.publication;drm_device_t *old_device=((drm_context_t *)fa->priv)->device;assert(!driver_unregister(&a.driver)&&class_device_binding_stopped(old));a.id=3;assert(!driver_register(&a.driver));a.publication=class_device_ref_for_device(&a.device);assert(a.publication!=old&&!class_device_binding_stopped(a.publication));fa2=open_sample(&a);assert(((drm_context_t *)fa2->priv)->device!=old_device);struct drm_mode_card_res res={0};unsigned reads=a.info_calls;assert(fa->ops->ioctl(fa,DRM_IOCTL_MODE_GETRESOURCES,&res)==-ENODEV&&a.info_calls==reads);assert(fa->ops->poll(fa,POLLIN)&POLLHUP);assert(dumb(fa2)==h);class_device_put(old);
   }else if(!strcmp(name,"read-race")){
    hook_sample=&a;on_link=withdraw_hook;unsigned char event[32];assert(fa->ops->read(fa,(char *)event,sizeof(event))==-ENODEV&&!on_link);
   }else if(!strcmp(name,"lease-drain")){
    pause_info=1;struct io_args args={fa,-1};pthread_t io,remove;assert(!pthread_create(&io,NULL,dimensions_thread,&args));assert(!pthread_mutex_lock(&pause_lock));while(!info_entered)assert(!pthread_cond_wait(&pause_changed,&pause_lock));assert(!pthread_mutex_unlock(&pause_lock));assert(!pthread_create(&remove,NULL,withdraw_thread,&a));while(__atomic_load_n(&a.publication->online,__ATOMIC_ACQUIRE))sched_yield();assert(!class_device_binding_stopped(a.publication));assert(!pthread_mutex_lock(&pause_lock));info_release=1;assert(!pthread_cond_broadcast(&pause_changed));assert(!pthread_mutex_unlock(&pause_lock));assert(!pthread_join(io,NULL)&&!pthread_join(remove,NULL)&&!args.result&&class_device_binding_stopped(a.publication));
   }else if(!strcmp(name,"fd-failure")){
    uint32_t h=dumb(fa);fd_install_error=-EMFILE;struct drm_prime_handle p={.handle=h,.flags=O_RDWR};assert(fa->ops->ioctl(fa,DRM_IOCTL_PRIME_HANDLE_TO_FD,&p)==-EMFILE);fd_install_error=0;close_gem(fa,h);
   }else if(!strcmp(name,"errors")){
    assert(fa->ops->ioctl(fa,DRM_IOCTL_MODE_GETRESOURCES,NULL)==-EFAULT);assert(fa->ops->ioctl(fa,0x12345678,NULL)==-EINVAL);assert(!a.publication->active_calls);drm_vfile_set_render_only(fa);struct drm_mode_card_res res={0};assert(fa->ops->ioctl(fa,DRM_IOCTL_MODE_GETRESOURCES,&res)==-EACCES&&!a.publication->active_calls);copied_fault=1;struct drm_version v={0};assert(fa->ops->ioctl(fa,DRM_IOCTL_VERSION,&v)==-EFAULT&&!a.publication->active_calls);copied_fault=0;
   }else assert(!strcmp(name,"initial"));
  }
  if(fa2)vfs_put_file(fa2);if(fa)vfs_put_file(fa);dispose_sample(&a);pump();assert(!g_drm_devices&&!vmo_live&&!page_live&&a.device.references==0);samples[1]=NULL;samples[2]=NULL;
 }
done:
 assert(heap_live==baseline+(g_drm_reaper_work?2:0));free(host_ram);while(mutexes){struct mutex_back *m=mutexes;mutexes=m->next;assert(!pthread_mutex_destroy(&m->native));free(m);}printf("DRM_BINDING: %s PASS\n",name);return 0;
}
