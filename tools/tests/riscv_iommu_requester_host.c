/* Production PCI/core, managed mapper and RISC-V provider; only hardware,
 * frames and scheduler are substituted by the including Python fixture. */
#include "mm/frame.h"
#include "drivers/core/dma_mapping.h"
#include "drivers/core/dma_buffer.h"
extern int arch_device_dma_ready(void) __attribute__((weak));
static uint8_t host_pages[96][PAGE_SIZE] __attribute__((aligned(4096)));
static unsigned host_refs[96], page_attempts, fail_page;
static unsigned regs[1024], commands, fail_commands_after, transient_errors;
static unsigned buffer_reserved, buffer_frees;
/* Mimic an order-4 allocation: only the head has a frame reference. */
pfn_t pfa_alloc(int order) {
 assert(order==4&&!buffer_reserved);buffer_reserved=1;host_refs[80]=1;return 80;
}
pfn_t pfa_alloc_max(int order,uint64_t limit) {assert(limit>=0x15ffff);return pfa_alloc(order);}
void pfa_free(pfn_t pfn,int order) {
 assert(pfn==80&&order==4&&buffer_reserved&&host_refs[80]==1);
 for(unsigned i=81;i<96;i++)assert(!host_refs[i]);
 buffer_frees++;host_refs[80]=0;buffer_reserved=0;
}
static pfn_t host_alloc_page(void) {
 if(++page_attempts==fail_page)return PFN_NONE;
 for(unsigned i=0;i<80;i++)if(!host_refs[i]){host_refs[i]=1;return i;}
 return PFN_NONE;
}
static void host_frame_get(pfn_t p) {assert(p<96&&host_refs[p]);host_refs[p]++;}
static void host_frame_put(pfn_t p) {assert(p<96&&host_refs[p]);host_refs[p]--;}
static void host_free_page(pfn_t p) {assert(p<96&&host_refs[p]==1);host_frame_put(p);}
static void *host_page_virt(pfn_t p) {assert(p<96);return host_pages[p];}
static paddr_t host_page_phys(pfn_t p) {assert(p<96);return 0x100000+(uint64_t)p*PAGE_SIZE;}
static pfn_t host_phys_page(paddr_t p) {return p>=0x100000&&p<0x160000?(p-0x100000)/PAGE_SIZE:PFN_NONE;}
static int host_page_valid(pfn_t p) {return p<96;}
static uint32_t host_readl(const volatile void *address) {
 size_t offset=(const volatile uint8_t *)address-(const volatile uint8_t *)regs;
 assert(offset<sizeof(regs)&&!(offset&3));
 if(offset==0x48) {
  if(transient_errors){transient_errors--;return 1U<<8;}
  if(fail_commands_after&&commands>=fail_commands_after)return 1U<<8;
 }
 return regs[offset/4];
}
static void host_writel(uint32_t value,volatile void *address) {
 size_t offset=(volatile uint8_t *)address-(volatile uint8_t *)regs;
 assert(offset<sizeof(regs)&&!(offset&3));regs[offset/4]=value;
 if(offset==0x24){commands++;regs[0x20/4]=value;}
}
void dma_sync_for_cpu(void *p,size_t n) {(void)p;(void)n;}
void dma_sync_for_device(void *p,size_t n) {(void)p;(void)n;}
#define pfa_alloc_page host_alloc_page
#define pfa_free_page host_free_page
#define pfn_to_virt host_page_virt
#define pfn_to_phys host_page_phys
#define phys_to_pfn host_phys_page
#define pfn_valid host_page_valid
#define frame_get host_frame_get
#define frame_put host_frame_put
#define readl host_readl
#define writel host_writel
/* PRODUCTION_BUFFER */
#include "drivers/core/riscv_iommu.c"
#undef readl
#undef writel
#include "drivers/core/dma_mapping.c"
#include "drivers/core/dma_policy.c"
int arch_device_dma_ready(void) {return 1;}
static uint8_t ddt[4096] __attribute__((aligned(4096)));
static uint64_t command_queue[512], fault_queue[512];
static unsigned live_frames(void) {
 unsigned n=0;for(unsigned i=0;i<96;i++)n+=!!host_refs[i];return n;
}
static void vacant(device_t *owner) {
 uint16_t rid=0;pci_requester_reservation_t *lease=NULL;
 assert(!pci_requester_reserve(owner,0,&rid,1,&lease));pci_requester_release(lease);
}
static void busy(device_t *owner) {
 uint16_t rid=0;pci_requester_reservation_t *lease=(void *)1;
 assert(pci_requester_reserve(owner,0,&rid,1,&lease)==-EBUSY&&!lease);
}
#include <time.h>
static void *host_shutdown(void *domain) {assert(!driver_dma_domain_shutdown(domain));return NULL;}
static pthread_barrier_t host_start, host_ready, host_finish;
static void host_barrier(pthread_barrier_t *barrier) {
 int r=pthread_barrier_wait(barrier);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);
}
struct host_contender { device_t *device; int result; bool user; };
static void *host_compete(void *opaque) {
 struct host_contender *c=opaque;pci_requester_reservation_t *lease=NULL;uint16_t rid=0;
 host_barrier(&host_start);
 c->result=c->user?riscv_iommu_domain_claim(0,42):pci_requester_reserve(c->device,0,&rid,1,&lease);
 host_barrier(&host_ready);host_barrier(&host_finish);
 if(c->user){if(!c->result)assert(!riscv_iommu_domain_release(0,42));}
 else pci_requester_release(lease);
 return NULL;
}
/* A partially initialized native consumer delegates rollback to remove. */
static struct driver_dma_domain *probe_domain;
static device_t *probe_child;
static unsigned probe_quiesces, probe_removes, probe_releases;
static int probe_child_error;
static void probe_release(void *context) {
 (void)context;assert(!probe_domain);probe_releases++;
}
static int native_probe(device_t *device) {
 assert(!device_probe_use_remove(device));
 assert(!device_add_resource_action(device,probe_release,NULL));
 int ret=riscv_iommu_dma_domain_create(device,&probe_domain);
 device->drv_priv=probe_domain;
 if(ret)return ret;
 if(probe_child)assert(!device_register(probe_child));
 return -EIO;
}
static void native_probe_quiesce(device_t *device) {(void)device;probe_quiesces++;}
static int native_probe_remove(device_t *device) {
 probe_removes++;
 int ret=probe_domain?driver_dma_domain_shutdown(probe_domain):0;
 if(ret)return ret;
 driver_dma_domain_put(probe_domain);probe_domain=NULL;device->drv_priv=NULL;return 0;
}
static int native_child_probe(device_t *device) {(void)device;return 0;}
static int native_child_remove(device_t *device) {(void)device;return probe_child_error;}
static int native_child_match(device_t *device,const driver_t *driver) {(void)device;(void)driver;return 1;}
static void native_probe_cleanup_case(device_t *device,device_t *external,bool child_pending) {
 bus_type_t bus={.name="native-probe-child",.match=native_child_match};
 driver_t child_driver={.name="native-probe-child",.bus=&bus,.probe=native_child_probe,.remove=native_child_remove};
 device_t child={.name="native-probe-child",.bus=&bus,.parent=device,.references=1};
 static const device_id_t ids[]={{.vendor=0x1234,.device=0x5678},{0}};
 driver_t driver={.name="native-probe",.id_table=ids,.bus=&pci_bus,.probe=native_probe,
  .quiesce=native_probe_quiesce,.remove=native_probe_remove};
 if(child_pending) {
  assert(!bus_register(&bus)&&!driver_register(&child_driver));
  probe_child=&child;probe_child_error=-ETIMEDOUT;
 } else fail_commands_after=3;
 assert(!driver_register(&driver));
 assert(device->drv==&driver&&device->drv_priv==probe_domain&&probe_domain);
 assert(device->probe_error==-EIO&&device_is_stopping(device)&&!probe_releases);
 busy(external);assert(live_frames()==3);
 if(child_pending) {
  assert(device->state==DEV_STATE_PROBE_CLEANUP&&!probe_quiesces&&!probe_removes);
  pfn_t page=host_alloc_page();unsigned before=commands;
  struct driver_dma_mapping *mapping=(void *)1;
  assert(driver_dma_map_pages(probe_domain,&page,1,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-ENODEV&&!mapping);
  assert(host_refs[page]==1&&commands==before);host_frame_put(page);
  struct driver_dma_domain *other=(void *)1;
  assert(riscv_iommu_dma_domain_create(device,&other)==-ENODEV&&!other);
  probe_child_error=0;
 } else {
  assert(device->state==DEV_STATE_REMOVING&&probe_quiesces==1&&probe_removes==1);
  assert(driver_unregister(&driver)==-EIO&&probe_quiesces==1&&probe_removes==2&&!probe_releases);
  assert(device->probe_error==-EIO);fail_commands_after=0;
 }
 assert(!device_unregister(device));
 assert(probe_quiesces==1&&probe_releases==1&&!probe_domain);
 assert(!live_frames());vacant(external);assert(!driver_unregister(&driver));
 if(child_pending) {
  assert(!device_unregister(&child));device_put(&child);
  assert(!driver_unregister(&child_driver));bus_unregister(&bus);
 }
}
static void requester_iommu_case(const char *name) {
 pci_insert();device_t *device=device_get(pci_current());assert(device);
 device_t external={.name="external",.references=1};assert(!device_register(&external));
 g_iommu_base=(uintptr_t)regs;g_iommu_ddt=ddt;g_iommu_dc_stride=32;
 g_iommu_cq=command_queue;g_iommu_fq=fault_queue;g_iommu_ready=1;
 struct driver_dma_domain *domain=NULL;uint16_t rid=0;
 if(!strcmp(name,"preparing")) {
  assert(!riscv_iommu_dma_domain_create(device,&domain));
  device->state=DEV_STATE_PREPARING;unsigned before=commands;
  pfn_t page=host_alloc_page();struct driver_dma_mapping *mapping=(void *)1;
  assert(driver_dma_map_pages(domain,&page,1,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-ENODEV&&!mapping);
  assert(host_refs[page]==1&&commands==before);host_frame_put(page);
  device_t *found=(void *)1;assert(pci_device_find_get(0,0,&found)==-ENODEV&&!found);
  busy(&external);assert(!driver_dma_domain_shutdown(domain));busy(&external);
  driver_dma_domain_put(domain);vacant(&external);device->state=DEV_STATE_PROBED;
 } else if(!strcmp(name,"probe-cleanup")||!strcmp(name,"probe-child")) {
  native_probe_cleanup_case(device,&external,!strcmp(name,"probe-child"));
 } else if(!strcmp(name,"external")) {
  pci_requester_reservation_t *lease=NULL;
  assert(!pci_requester_reserve(&external,0,&rid,1,&lease));
  assert(riscv_iommu_dma_domain_create(device,&domain)==-EBUSY&&!domain);
  cfg[1]|=4;assert(riscv_iommu_domain_claim(0,42)==-1);assert(cfg[1]&4);cfg[1]&=~4U;
  assert(!commands&&!live_frames()&&!ddt[0]);pci_requester_release(lease);
  assert(!riscv_iommu_dma_domain_create(device,&domain));busy(&external);
  assert(riscv_iommu_domain_claim(0,42)==-1);
  assert(!driver_dma_domain_shutdown(domain));busy(&external);driver_dma_domain_put(domain);vacant(&external);
 } else if(!strcmp(name,"user")) {
  cfg[1]|=4;assert(!riscv_iommu_domain_claim(0,42));assert(!(cfg[1]&4));unsigned first_commands=commands;
  cfg[1]|=4;assert(!riscv_iommu_domain_claim(0,42)&&commands==first_commands);assert(cfg[1]&4);cfg[1]&=~4U;busy(&external);
  assert(riscv_iommu_dma_domain_create(device,&domain)==-EBUSY&&!domain);
  assert(riscv_iommu_domain_claim(0,43)==-1);
  assert(!riscv_iommu_domain_release(0,42));vacant(&external);
 } else if(!strcmp(name,"lease-allocation")) {
  fail_at=allocations+1;assert(riscv_iommu_domain_claim(0,42)==-1);fail_at=0;
  assert(!commands&&!live_frames());vacant(&external);
 } else if(!strcmp(name,"native-allocations")) {
  for(unsigned i=1;i<=3;i++) {
   fail_at=allocations+i;assert(riscv_iommu_dma_domain_create(device,&domain)==-ENOMEM&&!domain);fail_at=0;
   assert(!commands&&!live_frames());vacant(&external);
  }
 } else if(!strcmp(name,"pages")) {
  for(unsigned i=1;i<=3;i++) {
   fail_page=page_attempts+i;assert(riscv_iommu_domain_claim(0,42)==-1);fail_page=0;
   assert(!commands&&!live_frames());vacant(&external);
  }
 } else if(!strcmp(name,"user-bme-read")) {
  cfg[1]=0xffffffff;assert(riscv_iommu_domain_claim(0,42)==-1);assert(cfg[1]==0xffffffff);cfg[1]=0;
  assert(!commands&&!live_frames());vacant(&external);
 } else if(!strcmp(name,"user-bme-write")) {
  cfg[1]|=4;reject_user_bme=1;assert(riscv_iommu_domain_claim(0,42)==-1);reject_user_bme=0;
  assert(cfg[1]&4);cfg[1]&=~4U;assert(!commands&&!live_frames());vacant(&external);
 } else if(!strcmp(name,"safe-failure")) {
  transient_errors=1;assert(riscv_iommu_domain_claim(0,42)==-1);
  assert(!live_frames());vacant(&external);
 } else if(!strcmp(name,"failed-claim")) {
  fail_commands_after=3;assert(riscv_iommu_domain_claim(0,42)==-1);
  assert(live_frames()==3);busy(&external);
  assert(riscv_iommu_domain_release(0,42)==-1);busy(&external);
  fail_commands_after=0;assert(!riscv_iommu_domain_claim(0,42));busy(&external);
  assert(!riscv_iommu_domain_release(0,42));vacant(&external);
 } else if(!strcmp(name,"native-failed-claim")) {
  fail_commands_after=3;assert(riscv_iommu_dma_domain_create(device,&domain)==-EIO&&domain);
  assert(live_frames()==3);busy(&external);
  assert(driver_dma_domain_shutdown(domain)==-EIO);busy(&external);
  fail_commands_after=0;assert(!driver_dma_domain_shutdown(domain));
  busy(&external);driver_dma_domain_put(domain);vacant(&external);
 } else if(!strcmp(name,"failed-release")) {
  assert(!riscv_iommu_dma_domain_create(device,&domain));
  pfn_t page=host_alloc_page();struct driver_dma_mapping *mapping=NULL;
  assert(!driver_dma_map_pages(domain,&page,1,DRIVER_DMA_BIDIRECTIONAL,&mapping));
  host_frame_put(page);assert(host_refs[page]==1);
  fail_commands_after=commands;
  assert(driver_dma_try_unmap_pages(&mapping)==-EIO&&mapping);busy(&external);
  assert(driver_dma_domain_shutdown(domain)==-EBUSY);assert(host_refs[page]==1);
  fail_commands_after=0;assert(!driver_dma_try_unmap_pages(&mapping));
  assert(!host_refs[page]);fail_commands_after=commands;
  assert(driver_dma_domain_shutdown(domain)==-EIO);busy(&external);
  assert(live_frames()==3);fail_commands_after=0;
  assert(!driver_dma_domain_shutdown(domain));busy(&external);driver_dma_domain_put(domain);vacant(&external);
 } else if(!strncmp(name,"buffer-",7)) {
  assert(!riscv_iommu_dma_domain_create(device,&domain));
  struct driver_dma_buffer *buffer;struct driver_dma_mapping *mapping=NULL;
  assert(!driver_dma_buffer_create(device,8193,65536,&buffer));
  assert(driver_dma_buffer_address(buffer)==0x150000&&host_refs[80]==1);
  if(!strcmp(name,"buffer-map-failure"))fail_commands_after=commands;
  int ret=driver_dma_map_buffer(domain,buffer,DRIVER_DMA_BIDIRECTIONAL,&mapping);
  assert(mapping&&((!strcmp(name,"buffer-map-failure"))?ret==-EIO:!ret));
  assert(driver_dma_mapping_count(mapping)==(!ret?3U:1U));
  for(unsigned i=80;i<96;i++)assert(host_refs[i]==(i==80?1U:0U));
  iommu_user_domain_t *hardware=&g_user_domains[0];
  for(unsigned i=0;i<driver_dma_mapping_count(mapping);i++)assert(hardware->pinned[i]==PFN_NONE);
  uint64_t rejected=1;unsigned before=commands;
  assert(riscv_iommu_domain_map(0,hardware->owner_pid,0x150000,1,&rejected)==-1&&!rejected);
  assert(riscv_iommu_domain_unmap(0,hardware->owner_pid,IOMMU_USER_IOVA,1)==-1&&commands==before);
  driver_dma_buffer_put(buffer);assert(buffer_reserved&&!buffer_frees);
  if(strcmp(name,"buffer-success")) {
   fail_commands_after=commands;
   assert(driver_dma_try_unmap_pages(&mapping)==-EIO&&mapping&&buffer_reserved);
   assert(driver_dma_domain_shutdown(domain)==-EBUSY);busy(&external);
   host_pages[80][0]=0x5c;host_pages[82][0]=0x6d;
   assert(!buffer_frees&&host_refs[80]==1);fail_commands_after=0;
  }
  assert(!driver_dma_try_unmap_pages(&mapping)&&!mapping&&!buffer_reserved&&buffer_frees==1);
  assert(live_frames()==3&&!host_refs[80]);
  assert(!driver_dma_domain_shutdown(domain));driver_dma_domain_put(domain);vacant(&external);
 } else if(!strcmp(name,"replacement")) {
  assert(!riscv_iommu_domain_claim(0,42));
  cfg[0]=0x43211234;pci_rescan();assert(pci_current()!=device);
  assert(riscv_iommu_domain_claim(0,42)==-1);busy(&external);
  assert(riscv_iommu_dma_domain_create(device,&domain)==-ESTALE&&!domain);
  assert(riscv_iommu_dma_domain_create(pci_current(),&domain)==-EBUSY&&!domain);
  assert(!riscv_iommu_domain_release(0,42));vacant(&external);
 } else if(!strcmp(name,"lock-order")) {
  assert(!riscv_iommu_dma_domain_create(device,&domain));
  driver_discovery_begin();pthread_t thread;assert(!pthread_create(&thread,NULL,host_shutdown,domain));
  struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=2;
  /* Shutdown must not wait on discovery while holding the managed mutex. */
  assert(!pthread_timedjoin_np(thread,NULL,&deadline));busy(&external);
  assert(!driver_dma_domain_shutdown(domain));driver_discovery_end();
  driver_dma_domain_put(domain);vacant(&external);
 } else if(!strcmp(name,"concurrent")) {
  assert(!pthread_barrier_init(&host_start,NULL,3));
  assert(!pthread_barrier_init(&host_ready,NULL,3));
  assert(!pthread_barrier_init(&host_finish,NULL,3));
  for(unsigned i=0;i<64;i++) {
   struct host_contender a={.device=device,.user=true},b={.device=&external};pthread_t x,y;
   assert(!pthread_create(&x,NULL,host_compete,&a)&&!pthread_create(&y,NULL,host_compete,&b));
   host_barrier(&host_start);host_barrier(&host_ready);
   assert((!a.result&&b.result==-EBUSY)||(a.result==-1&&!b.result));busy(&external);
   host_barrier(&host_finish);assert(!pthread_join(x,NULL)&&!pthread_join(y,NULL));
   assert(!live_frames()&&!pci_requester_reservations);
  }
  assert(!pthread_barrier_destroy(&host_start)&&!pthread_barrier_destroy(&host_ready));
  assert(!pthread_barrier_destroy(&host_finish));
 } else if(!strcmp(name,"removed")) {
  assert(!riscv_iommu_domain_claim(0,42));pci_remove();busy(&external);
  assert(riscv_iommu_domain_claim(0,42)==-1);
  assert(!riscv_iommu_domain_release(0,42));vacant(&external);
 } else assert(0);
 assert(!live_frames()&&!pci_requester_reservations);
 for(unsigned i=0;i<IOMMU_USER_DOMAINS;i++)assert(!g_user_domains[i].active);
 device_unregister(&external);assert(external.references==1);
 device_put(device);if(pci_current())pci_remove();
}
