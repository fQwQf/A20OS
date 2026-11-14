/* Native queue core + real fences; hardware, work scheduling and immutable
 * memory backing are host models. Tests exercise whole request workflows. */
#include "drivers/core/queue.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/dma_buffer.h"
#include "drivers/core/workqueue.h"
#include "mm/vmo.h"
static int (*binding_stop)(void *);
static void (*binding_release)(void *);
static void *binding_context;
void *kcalloc(size_t n, size_t z) { void *p = kmalloc(n*z); if (p) memset(p, 0, n*z); return p; }
int device_probe_use_remove(device_t *d) { (void)d; return 0; }
int device_add_execution_action(device_t *d, int (*stop)(void *), void (*release)(void *), void *ctx)
{ (void)d; binding_stop=stop; binding_release=release; binding_context=ctx; return 0; }
struct driver_dma_buffer { unsigned refs; size_t size; };
void driver_dma_buffer_get(struct driver_dma_buffer *b) { if(b)b->refs++; }
void driver_dma_buffer_put(struct driver_dma_buffer *b) { assert(!host_lock_depth&&!irq_context); if(b){assert(b->refs);b->refs--;} }
size_t driver_dma_buffer_size(const struct driver_dma_buffer *b) { return b->size; }
void vmo_ref(struct vmo *v) { if(v)refcount_inc(&v->refcount); }
void vmo_release(struct vmo *v) { assert(!host_lock_depth&&!irq_context);if(v)assert(!refcount_dec_and_test(&v->refcount)); }
struct driver_work { void (*callback)(void *); void *context; bool pending; uint64_t deadline; };
static struct driver_work *work;
struct driver_work *driver_work_create(struct driver_workqueue *q, void (*cb)(void *), void *p)
{ (void)q;work=kcalloc(1,sizeof(*work));if(work){work->callback=cb;work->context=p;}return work; }
int driver_work_schedule(struct driver_work *w, uint64_t d) { if(w->pending)return 0;w->pending=true;w->deadline=d;return 1; }
int driver_work_reschedule(struct driver_work *w,uint64_t d){w->pending=true;w->deadline=d;return 1;}
int driver_work_cancel_sync(struct driver_work *w, uint64_t d) { (void)d;assert(!host_lock_depth);w->pending=false;return 0; }
void driver_work_put(struct driver_work *w) { assert(w==work);work=NULL;kfree(w); }
static void progress(void) { assert(work&&work->pending);work->pending=false;work->callback(work->context); }
struct backend {
 unsigned prepared,issued,retired,stops;
 int prepare_error,retire_error,stop_error;
 bool inline_complete,partial_prepare,reentrant_stop;
 struct driver_request *requests[16];
 struct driver_queue *queue;
};
static int prepare_request(void *p,struct driver_request *r)
{
 struct backend *b=p;assert(!host_lock_depth&&!irq_context);b->prepared++;
 if(b->partial_prepare||!b->prepare_error)driver_request_set_private(r,(void *)0x55);
 return b->prepare_error;
}
static void issue_request(void *p,struct driver_request *r)
{
 struct backend *b=p;assert(!host_lock_depth&&!irq_context);b->requests[b->issued++]=r;
 if(b->reentrant_stop)assert(driver_queue_stop(b->queue)==-EBUSY);
 if(b->inline_complete)assert(!driver_request_complete(r,0));
}
static int retire_request(void *p,struct driver_request *r)
{
 struct backend *b=p;assert(!host_lock_depth&&!irq_context);b->retired++;
 if(b->retire_error&&driver_request_private(r))return b->retire_error;
 driver_request_set_private(r,NULL);return 0;
}
static int stop_backend(void *p)
{ struct backend *b=p;assert(!host_lock_depth&&!irq_context);b->stops++;return b->stop_error; }
static struct driver_queue *new_queue(struct backend *b,unsigned depth,unsigned active,uint64_t bytes)
{
 struct driver_queue *q;device_t dev={0};
 struct driver_queue_limits limits={depth,active,16,bytes};
 struct driver_queue_ops ops={.size=sizeof(ops),.prepare=prepare_request,.issue=issue_request,.retire=retire_request,.stop=stop_backend};
 assert(!driver_queue_create(&dev,&limits,&ops,b,&q));b->queue=q;return q;
}
static struct driver_request *new_request(struct driver_queue *q,unsigned command,struct driver_fence *dep)
{
 struct driver_request *r;struct driver_request_description d={.command=&command,.command_size=sizeof(command),.dependency=dep};
 assert(!driver_request_create(q,&d,&r));return r;
}
static void finish_queue(struct driver_queue *q)
{ assert(!binding_stop(binding_context));binding_release(binding_context);driver_queue_put(q);assert(!allocations); }
static void hardware_done(struct driver_request *r,int error)
{ unsigned n=allocations;irq_context=true;assert(!driver_request_complete(r,error));irq_context=false;assert(allocations==n); }
static void fence_reentry(void *ctx,int status)
{
 struct driver_queue *q=ctx;assert(status==1&&!host_lock_depth);assert(driver_queue_run(q,1)>=0);
}
static void workflow_parallel(void)
{
 struct backend b={0};struct driver_queue *q=new_queue(&b,3,2,12);
 struct driver_request *a=new_request(q,1,NULL),*c=new_request(q,2,NULL),*d=new_request(q,3,NULL),*e=new_request(q,4,NULL);
 assert(!driver_request_submit(a)&&!driver_request_submit(c)&&!driver_request_submit(d));
 assert(driver_request_submit(e)==-EAGAIN);assert(driver_queue_run(q,16)==2&&b.issued==2);
 struct driver_fence_watch *watch;assert(!driver_fence_watch_create(driver_request_fence(c),fence_reentry,q,&watch));
 hardware_done(c,0);assert(!driver_fence_status(driver_request_fence(c)));
 assert(driver_queue_run(q,16)==2&&b.issued==3&&driver_fence_status(driver_request_fence(c))==1);
 assert(!driver_fence_watch_try_remove(&watch));assert(!driver_request_submit(e));
 hardware_done(a,-EIO);hardware_done(d,0);assert(driver_request_complete(d,0)==-EALREADY);
 assert(driver_queue_run(q,16)==3&&b.issued==4);hardware_done(e,0);assert(driver_queue_run(q,16)==1);
 struct driver_queue_status s;driver_queue_status(q,&s);assert(!s.outstanding&&!s.active&&!s.bytes);
 driver_request_put(a);driver_request_put(c);driver_request_put(d);driver_request_put(e);finish_queue(q);
}
static void workflow_cancel_dependency(void)
{
 struct backend b={0};struct driver_queue *q=new_queue(&b,4,2,16);
 struct driver_fence *dep=driver_fence_create();
 struct driver_request *a=new_request(q,1,dep),*c=new_request(q,2,NULL),*d=new_request(q,3,dep);
 assert(!driver_request_submit(a)&&!driver_request_submit(c)&&!driver_request_submit(d));
 assert(driver_queue_run(q,16)==0&&!b.issued);assert(!driver_request_cancel(c));
 assert(driver_queue_run(q,16)==1&&driver_fence_status(driver_request_fence(c))==-ECANCELED);
 assert(!driver_fence_signal(dep,-EIO));assert(driver_queue_run(q,16)==4&&!b.prepared&&!b.issued);
 assert(driver_fence_status(driver_request_fence(a))==-EIO&&driver_fence_status(driver_request_fence(d))==-EIO);
 driver_request_put(a);driver_request_put(c);driver_request_put(d);driver_fence_put(dep);finish_queue(q);
}
static void workflow_retirement(void)
{
 struct backend b={.retire_error=-EIO};struct driver_queue *q=new_queue(&b,2,2,8);
 struct driver_request *a=new_request(q,1,NULL),*c=new_request(q,2,NULL);
 assert(!driver_request_submit(a)&&!driver_request_submit(c)&&driver_queue_run(q,16)==2);
 assert(driver_request_cancel(a)==-EBUSY);hardware_done(a,0);hardware_done(c,0);
 assert(driver_queue_run(q,16)==-EIO&&!driver_fence_status(driver_request_fence(a))&&!driver_fence_status(driver_request_fence(c)));
 struct driver_queue_status s;driver_queue_status(q,&s);assert(s.outstanding==2&&s.active==2&&s.bytes==8);
 b.stop_error=-ETIMEDOUT;assert(binding_stop(binding_context)==-ETIMEDOUT);
 assert(!driver_fence_status(driver_request_fence(a))&&b.stops==1);
 b.stop_error=0;assert(binding_stop(binding_context)==-EIO&&b.stops==2);
 b.retire_error=0;assert(!binding_stop(binding_context)&&b.stops==2);
 assert(driver_fence_status(driver_request_fence(a))==1&&driver_fence_status(driver_request_fence(c))==1);
 binding_release(binding_context);driver_queue_put(q);
 assert(driver_request_submit(a)==-EALREADY);assert(driver_request_cancel(c)==-EALREADY);
 driver_request_put(a);driver_request_put(c);assert(!allocations);
}
static void workflow_prepare_worker(void)
{
 struct backend b={.prepare_error=-EAGAIN,.inline_complete=true};struct driver_queue *q=new_queue(&b,2,2,8);
 assert(!driver_queue_start(q,(void *)1));struct driver_request *a=new_request(q,1,NULL);
 assert(!driver_request_submit(a));progress();assert(!b.issued&&work->pending&&work->deadline);
 b.prepare_error=-EINVAL;b.partial_prepare=true;progress();assert(driver_fence_status(driver_request_fence(a))==-EINVAL&&!b.issued&&b.retired==1);
 b.prepare_error=0;b.partial_prepare=false;struct driver_request *c=new_request(q,2,NULL);
 assert(!driver_request_submit(c));progress();assert(driver_fence_status(driver_request_fence(c))==1&&b.issued==1);
 driver_request_put(a);driver_request_put(c);finish_queue(q);
}
static void workflow_memory_stop(void)
{
 struct backend b={0};struct driver_queue *q=new_queue(&b,2,2,8192);
 struct vmo v={.type=VMO_RESIDENT,.size=4096,.refcount=REFCOUNT_INIT(1)};
 struct driver_dma_buffer buffer={.refs=1,.size=4096};
 struct driver_request_memory memory[2]={{.type=DRIVER_REQUEST_VMO,.owner.vmo=&v,.offset=100,.length=200},
  {.type=DRIVER_REQUEST_DMA_BUFFER,.owner.buffer=&buffer,.offset=1,.length=300}};
 unsigned command=42;struct driver_request_description desc={.command=&command,.command_size=4,.memory=memory,.memory_count=2};
 struct driver_request *r;assert(!driver_request_create(q,&desc,&r));command=99;memory[0].length=1;
 size_t n;assert(*(const unsigned *)driver_request_command(r,&n)==42&&n==4);
 assert(driver_request_memory(r,&n)[0].length==200&&n==2&&refcount_read(&v.refcount)==2&&buffer.refs==2);
 struct driver_fence *f=driver_request_fence(r);driver_fence_get(f);
 assert(!driver_request_submit(r)&&driver_queue_run(q,16)==1);driver_request_put(r);
 b.stop_error=-EIO;assert(binding_stop(binding_context)==-EIO&&!driver_fence_status(f));
 assert(refcount_read(&v.refcount)==2&&buffer.refs==2);
 b.stop_error=0;assert(!binding_stop(binding_context)&&driver_fence_status(f)==-ESHUTDOWN);
 assert(refcount_read(&v.refcount)==1&&buffer.refs==1);binding_release(binding_context);
 driver_fence_put(f);driver_queue_put(q);assert(!allocations);
}
static void workflow_dispatch_race(void)
{
 struct backend b={.reentrant_stop=true};struct driver_queue *q=new_queue(&b,2,2,8);
 struct driver_request *a=new_request(q,1,NULL),*c=new_request(q,2,NULL);
 assert(!driver_request_submit(a)&&!driver_request_submit(c));assert(driver_queue_run(q,16)==1&&b.issued==1&&!b.stops);
 struct driver_queue_status s;driver_queue_status(q,&s);assert(s.closing&&s.outstanding==2);
 assert(!binding_stop(binding_context)&&b.stops==1);
 assert(driver_fence_status(driver_request_fence(a))==-ESHUTDOWN&&driver_fence_status(driver_request_fence(c))==-ESHUTDOWN);
 driver_request_put(a);driver_request_put(c);binding_release(binding_context);driver_queue_put(q);assert(!allocations);
}
static void workflow_dependency_notifications(void)
{
 struct backend b={.inline_complete=true};struct driver_queue *q=new_queue(&b,2,2,8);
 assert(!driver_queue_start(q,(void *)1));
 struct driver_fence *dep=driver_fence_create();struct driver_request *a=new_request(q,1,dep);
 assert(!driver_request_submit(a));progress();assert(!work->pending&&!b.issued);
 unsigned n=allocations;irq_context=true;assert(!driver_fence_signal(dep,0));irq_context=false;
 assert(allocations==n&&work->pending&&!work->deadline);progress();
 assert(driver_fence_status(driver_request_fence(a))==1&&b.issued==1);
 driver_request_put(a);driver_fence_put(dep);
 dep=driver_fence_create();a=new_request(q,2,dep);assert(!driver_request_submit(a));
 progress();assert(!work->pending);assert(!driver_request_cancel(a));progress();
 assert(driver_fence_status(driver_request_fence(a))==-ECANCELED&&!work->pending);
 driver_request_put(a);assert(!driver_fence_signal(dep,0)&&!work->pending);
 driver_fence_put(dep);finish_queue(q);
}
int main(int argc,char **argv)
{
 assert(argc==2);if(!strcmp(argv[1],"parallel"))workflow_parallel();
 else if(!strcmp(argv[1],"cancel"))workflow_cancel_dependency();
 else if(!strcmp(argv[1],"retire"))workflow_retirement();
 else if(!strcmp(argv[1],"worker"))workflow_prepare_worker();
 else if(!strcmp(argv[1],"memory"))workflow_memory_stop();
 else if(!strcmp(argv[1],"dependency-wake"))workflow_dependency_notifications();
 else {assert(!strcmp(argv[1],"race"));workflow_dispatch_race();}
 assert(!allocations&&!linked_count);return 0;
}
