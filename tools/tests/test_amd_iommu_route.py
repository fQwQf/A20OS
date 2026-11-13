"""Retained AMD route objects with production PCI/core, owner and IVRS policy.

The hardware matrix, memory allocator and scheduler are host substitutes.
No fixture substitutes routing, capability validation or lifetime decisions.
"""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

if __package__:
    from .test_native_bus_lifetime import ROOT, C_FIXTURE
    from .test_pci_scope import HARDWARE, CASES as SCOPE_CASES
else:
    from test_native_bus_lifetime import ROOT, C_FIXTURE
    from test_pci_scope import HARDWARE, CASES as SCOPE_CASES


WRAPPERS = r'''
static int checked_acquire(device_t *client,amd_iommu_route_t **out) {
 unsigned n=scope_writes;int ret=amd_iommu_route_acquire(client,out);assert(scope_writes==n);return ret;
}
static int checked_validate(amd_iommu_route_t *route,amd_iommu_route_info_t *out) {
 unsigned n=scope_writes;int ret=amd_iommu_route_validate(route,out);assert(scope_writes==n);return ret;
}
static int checked_memory(amd_iommu_route_t *route,size_t index,acpi_ivrs_memory_t *out) {
 unsigned n=scope_writes;int ret=amd_iommu_route_memory(route,index,out);assert(scope_writes==n);return ret;
}
static int checked_reserve(amd_iommu_route_t *route) {
 unsigned n=scope_writes;int ret=amd_iommu_route_reserve(route);assert(scope_writes==n);return ret;
}
static int checked_policy_create(amd_iommu_route_t *route,struct driver_dma_policy **out) {
 unsigned n=scope_writes;int ret=amd_iommu_route_policy_create(route,out);assert(scope_writes==n);return ret;
}
static int checked_requesters(amd_iommu_route_t *route,uint16_t *ids,size_t capacity,size_t *count) {
 unsigned n=scope_writes;int ret=amd_iommu_route_requesters(route,ids,capacity,count);assert(scope_writes==n);return ret;
}
static void checked_release(amd_iommu_route_t *route) {
 unsigned n=scope_writes;amd_iommu_route_release(route);assert(scope_writes==n);
}
'''

CASES = r'''
#include "drivers/core/amd_iommu_owner.c"
static uint8_t route_xsdt[44], route_ivrs[512];
static size_t route_length;
static int route_tables, route_maps, route_unmaps;
static acpi_context_t route_acpi;
static device_t route_peer={.name="peer",.references=1};
static void route_put(uint8_t *p,unsigned offset,uint64_t value,unsigned n) {
 for(unsigned i=0;i<n;i++)p[offset+i]=value>>(8*i);
}
static void route_checksum(uint8_t *p,size_t n) {
 p[9]=0;unsigned sum=0;for(size_t i=0;i<n;i++)sum+=p[i];p[9]=(uint8_t)-sum;
}
static const void *route_map_table(void *context,uint64_t address,size_t n) {
 (void)context;const void *p=NULL;
 if(address==0x1000&&n<=sizeof(route_xsdt))p=route_xsdt;
 if(address==0x2000&&n<=route_length)p=route_ivrs;
 if(p)route_tables++;
 return p;
}
static void route_unmap_table(void *context,const void *p,size_t n) {
 (void)context;(void)n;assert((p==route_xsdt||p==route_ivrs)&&route_tables>0);route_tables--;
}
const acpi_context_t *arch_acpi_context(void) {return &route_acpi;}
static void route_hw_map_check(void) {
 resource_t range={.type=RES_MMIO,.start=0xfed80000,.end=0xfed83fff};
 assert(route_tables>0&&driver_resource_check_claims(&route_peer,&range)==-EBUSY);
}
static void *route_map(uint64_t pa,size_t n,uint32_t flags) {
 (void)flags;assert(pa==0xfed80000&&n==0x4000);route_hw_map_check();route_maps++;return mmio;
}
static void route_unmap(void *p,size_t n) {
 assert(p==mmio&&n==0x4000);route_hw_map_check();route_unmaps++;
}
static void route_select(unsigned requester,unsigned settings) {
 route_ivrs[route_length]=2;route_put(route_ivrs,route_length+1,requester,2);
 route_ivrs[route_length+3]=settings;route_length+=4;
}
static void route_ivmd(unsigned requester,uint64_t start,unsigned flags) {
 size_t at=route_length;route_ivrs[at]=0x21;route_ivrs[at+1]=flags;
 route_put(route_ivrs,at+2,32,2);route_put(route_ivrs,at+4,requester,2);
 route_put(route_ivrs,at+16,start,8);route_put(route_ivrs,at+24,0x2000,8);route_length+=32;
}
static void route_firmware(int alias) {
 memcpy(route_xsdt,"XSDT",4);route_put(route_xsdt,4,sizeof(route_xsdt),4);route_xsdt[8]=1;
 route_put(route_xsdt,36,0x2000,8);route_checksum(route_xsdt,sizeof(route_xsdt));
 memcpy(route_ivrs,"IVRS",4);route_ivrs[8]=2;route_ivrs[36]=1;route_length=88;
 route_ivrs[48]=0x11;route_put(route_ivrs,52,0x10,2);route_put(route_ivrs,54,0x40,2);
 route_put(route_ivrs,56,0xfed80000,8);
 if(alias) {
  route_ivrs[route_length]=0x42;route_put(route_ivrs,route_length+1,0x21a,2);
  route_ivrs[route_length+3]=0x13;route_put(route_ivrs,route_length+5,0x220,2);route_length+=8;
  route_select(0x220,0x27);
 } else route_select(0x21a,0x13);
 route_put(route_ivrs,50,route_length-48,2);
 route_ivmd(0x21a,0x100000,7);if(alias)route_ivmd(0x220,0x200000,5);
 route_put(route_ivrs,4,route_length,4);route_checksum(route_ivrs,route_length);
 route_acpi=(acpi_context_t){.mapper={.map=route_map_table,.unmap=route_unmap_table},.root=0x1000,.entry_width=8};
}
static void route_error(amd_iommu_route_t *route,int expected) {
 amd_iommu_route_info_t info;memset(&info,0xa5,sizeof(info));
 assert(amd_iommu_route_validate(route,&info)==expected);
 assert(!info.requester&&!info.unit.base&&!info.memory_count&&!info.alias);
}
static void route_acquire_error(device_t *client,int expected) {
 amd_iommu_route_t *route=(void *)1;
 assert(amd_iommu_route_acquire(client,&route)==expected&&!route);
}
static size_t route_memory_offset(unsigned index) {
 for(size_t at=48;at<route_length;at+=acpi_le16(route_ivrs+at+2))
  if(route_ivrs[at]>=0x20&&route_ivrs[at]<=0x22) {if(!index--)return at;}
 assert(0);return 0;
}
static void route_policy_error(amd_iommu_route_t *route,int expected) {
 struct driver_dma_policy *p=(void *)1;int before=allocated;
 assert(amd_iommu_route_policy_create(route,&p)==expected&&!p&&allocated==before);
}
static void route_policy_check(struct driver_dma_policy *p,uint64_t address,
                               enum driver_dma_direction direction,int error,bool expected) {
 bool identity=!expected;
 assert(driver_dma_policy_check(p,address,0x1000,direction,&identity)==error&&identity==expected);
}
static void route_case(const char *name) {
 int alias=strstr(name,"alias")!=NULL,valid=strstr(name,"range")!=NULL;
 int peer_case=!strncmp(name,"peers-",6);
 unsigned peer_requester=0x228;
 struct scope_function *peer_hw=NULL;
 struct scope_function *root=scope_bridge(0,1,1,2);
 scope_bridge(1,2,2,2);scope_add(2,3,2,0);
 if(alias)scope_add(2,4,0,0);
 struct scope_function *controller=scope_add(0,2,0,0);
 controller->config[0]=0x14191022;controller->config[2]=0x08060000;
 controller->config[1]=1U<<20;controller->config[0x34/4]=0x40;
 controller->config[0x40/4]=0x0f|(3U<<16)|(1U<<19)|(1U<<27);
 controller->config[0x44/4]=0xfed80001; /* Enabled, read-only MMIO observation. */
 controller->config[0x4c/4]=valid?0x08080080:0xff000000;
 if(!strcmp(name,"range-missing"))controller->config[0x4c/4]=0x09090080;
 controller->config[0x50/4]=(40U<<8)|(48U<<15);
 route_firmware(alias);
 unsigned expected_memory_count=alias?2:1;
 unsigned first_flags=7,second_flags=5;uint64_t second_start=0x200000;
 if(!strcmp(name,"policy-deny"))first_flags=0;
 if(!strcmp(name,"policy-read"))first_flags=2;
 if(!strcmp(name,"policy-unity-read"))first_flags=3;
 if(!strcmp(name,"policy-write"))first_flags=4;
 if(!strcmp(name,"policy-linear"))first_flags=6;
 if(!strcmp(name,"policy-exclusion"))first_flags=15;
 if(!strcmp(name,"policy-reserved"))first_flags=0x87;
 if(!strcmp(name,"policy-invalid-unity"))first_flags=1;
 route_ivrs[route_memory_offset(0)+1]=first_flags;
 if(!strcmp(name,"policy-alias-overlap")) {
  size_t at=route_memory_offset(1);second_flags=2;second_start=0x100800;
  route_ivrs[at+1]=second_flags;route_put(route_ivrs,at+16,second_start,8);
  route_put(route_ivrs,at+24,0x800,8);
 }
 if(!strcmp(name,"policy-alternatives")) {
  /* The selected 11h IVHD must not hide restrictions beneath its legacy
   * 10h alternative, which describes the very same controller/capability. */
  size_t at=route_length;route_ivrs[at]=0x10;route_put(route_ivrs,at+2,28,2);
  route_put(route_ivrs,at+4,0x10,2);route_put(route_ivrs,at+6,0x40,2);
  route_put(route_ivrs,at+8,0xfed80000,8);route_length+=24;route_select(0x21a,0x13);
  route_ivmd(0x21a,0x100800,2);expected_memory_count=2;
 }
 if(!strcmp(name,"reserve-alias-sibling")||peer_case) {
  if(strstr(name,"range-outside")) {
   scope_bridge(0,4,3,3);peer_requester=0x328;peer_hw=scope_add(3,5,0,0);
  } else peer_hw=scope_add(2,5,0,0);
  memmove(route_ivrs+108,route_ivrs+100,route_length-100);
  memset(route_ivrs+100,0,8);route_ivrs[100]=0x42;
  route_put(route_ivrs,101,peer_requester,2);route_put(route_ivrs,105,0x220,2);
  route_length+=8;route_put(route_ivrs,50,60,2);
  if(peer_case)route_ivmd(peer_requester,0x300000,strstr(name,"exclusion")?8:0);
 }
 if(!strcmp(name,"alias-range-outside")) {
  scope_hw[2][4][0].present=0;scope_add(0,4,0,0);
  route_put(route_ivrs,93,0x20,2);route_put(route_ivrs,97,0x20,2);
 }
 if(!strcmp(name,"ambiguous")) {
  size_t at=route_length;route_ivrs[at]=0x11;route_put(route_ivrs,at+2,44,2);
  route_put(route_ivrs,at+4,0x10,2);route_put(route_ivrs,at+6,0x80,2);
  route_put(route_ivrs,at+8,0xfedc0000,8);route_length+=40;route_select(0x21a,0);
  controller->config[0x40/4]|=0x80<<8;
  controller->config[0x80/4]=0x0f|(3U<<16)|(1U<<19)|(1U<<27);
  controller->config[0x84/4]=0xfedc0000;controller->config[0x90/4]=(40U<<8)|(48U<<15);
 }
 route_put(route_ivrs,4,route_length,4);route_checksum(route_ivrs,route_length);
 pci_enumerate(0,0,1);
 device_t *client=scope_device(0x21a),*provider=scope_device(0x10);
 if(!strcmp(name,"unbound")) {
  route_acquire_error(client,-ENODEV);goto cleanup;
 }
 driver_discovery_begin();assert(!driver_register(&amdvi_driver));driver_discovery_end();
 assert(!bus_probe_device(provider)&&provider->drv==&amdvi_driver&&route_tables==1);
 int before=allocated;unsigned client_refs=client->references;
 if(!strcmp(name,"arguments")) {
  route_acquire_error(NULL,-EINVAL);assert(amd_iommu_route_acquire(client,NULL)==-EINVAL);
  route_error(NULL,-EINVAL);
  assert(amd_iommu_route_validate(NULL,NULL)==-EINVAL);
  assert(amd_iommu_route_reserve(NULL)==-EINVAL);
  route_policy_error(NULL,-EINVAL);assert(amd_iommu_route_policy_create(NULL,NULL)==-EINVAL);
  acpi_ivrs_memory_t m;memset(&m,0xa5,sizeof(m));
  assert(amd_iommu_route_memory(NULL,0,&m)==-EINVAL&&!m.start);
  assert(amd_iommu_route_memory(NULL,0,NULL)==-EINVAL);amd_iommu_route_release(NULL);
  size_t n=999;uint16_t ids[2]={0xa5a5,0xa5a5};
  assert(amd_iommu_route_requesters(NULL,ids,2,&n)==-EINVAL&&!n&&ids[0]==0xa5a5);
  assert(amd_iommu_route_requesters(NULL,ids,2,NULL)==-EINVAL);
 } else if(!strcmp(name,"allocation")) {
  fail_at=allocations+1;route_acquire_error(client,-ENOMEM);fail_at=0;
  assert(client->references==client_refs&&allocated==before&&route_tables==1);
 } else if(!strcmp(name,"alias-missing")) {
  scope_hw[2][4][0].present=0;pci_rescan();route_acquire_error(client,-ENODEV);
 } else if(!strcmp(name,"range-missing")) {
  route_acquire_error(client,-ENODEV);
 } else if(!strcmp(name,"range-root-type")) {
  root->config[0x40/4]=0x10|(2U<<16)|(6U<<20);route_acquire_error(client,-EOPNOTSUPP);
 } else if(!strcmp(name,"alias-range-outside")) {
  route_acquire_error(client,-ENODEV);
 } else if(!strcmp(name,"ambiguous")) {
  route_acquire_error(client,-EBUSY);
 } else {
  amd_iommu_route_t *route=NULL;assert(!amd_iommu_route_acquire(client,&route)&&route);
  assert(!client->dependencies&&!provider->dependencies);
  amd_iommu_route_info_t info;assert(!amd_iommu_route_validate(route,&info));
  assert(info.requester==0x21a&&info.dma_requester==(alias?0x220:0x21a));
  assert(info.unit.requester==0x10&&info.unit.base==0xfed80000&&info.settings==0x13);
  assert(info.alias==alias&&info.hardware_range_checked==valid&&info.memory_count==expected_memory_count);
  acpi_ivrs_memory_t memory;
  assert(!amd_iommu_route_memory(route,0,&memory)&&memory.start==0x100000&&memory.flags==first_flags);
  if(alias)assert(!amd_iommu_route_memory(route,1,&memory)&&memory.start==second_start&&memory.flags==second_flags);
  assert(amd_iommu_route_memory(route,info.memory_count,&memory)==-ENODEV&&!memory.start);
  if(peer_case) {
   size_t count=0;uint16_t ids[ACPI_IVRS_REQUESTER_MAX];
   for(unsigned i=0;i<ACPI_IVRS_REQUESTER_MAX;i++)ids[i]=0xa5a5;
   assert(amd_iommu_route_requesters(route,NULL,0,&count)==-ENOSPC&&count==3);
   assert(amd_iommu_route_requesters(route,ids,2,&count)==-ENOSPC&&count==3&&ids[0]==0xa5a5);
   assert(amd_iommu_route_requesters(route,NULL,3,&count)==-EINVAL&&!count);
   assert(!amd_iommu_route_requesters(route,ids,ACPI_IVRS_REQUESTER_MAX,&count)&&count==3);
   assert(ids[0]==0x21a&&ids[1]==0x220&&ids[2]==peer_requester&&ids[3]==0xa5a5);
   device_t *member=scope_device(peer_requester);
   if(strstr(name,"policy")) {
    if(strstr(name,"exclusion"))route_policy_error(route,-EOPNOTSUPP);
    else {
     struct driver_dma_policy *p=NULL;assert(!amd_iommu_route_policy_create(route,&p));
     route_policy_check(p,0x300000,DRIVER_DMA_TO_DEVICE,-EACCES,false);
     route_policy_check(p,0x100000,DRIVER_DMA_TO_DEVICE,0,true);driver_dma_policy_put(p);
    }
   } else {
    int before=allocated;unsigned refs=member->references;
    if(strstr(name,"missing")) {
     peer_hw->present=0;pci_rescan();assert(amd_iommu_route_reserve(route)==-ENODEV);
     peer_hw->present=1;pci_rescan();device_put(member);member=scope_device(peer_requester);
    } else if(strstr(name,"range-outside")) {
     assert(amd_iommu_route_reserve(route)==-ENODEV);goto peer_done;
    } else if(strstr(name,"user-before")) {
     member->user_owned=1;assert(amd_iommu_route_reserve(route)==-EBUSY);member->user_owned=0;
    } else if(strstr(name,"bme")) {
     peer_hw->config[1]|=4;assert(amd_iommu_route_reserve(route)==-EBUSY);peer_hw->config[1]&=~4U;
    } else if(strstr(name,"read")) {
     peer_hw->config[1]=0xffffffff;assert(amd_iommu_route_reserve(route)==-EIO);peer_hw->config[1]=0;
    } else if(strstr(name,"allocation")) {
     for(unsigned i=1;i<=2;i++) {
      fail_at=allocations+i;assert(amd_iommu_route_reserve(route)==-ENOMEM);fail_at=0;
      assert(allocated==before&&member->references==refs);
     }
    } else if(strstr(name,"conflict")) {
     pci_requester_reservation_t *block=NULL,*vacant=NULL;
     uint16_t key=peer_requester,original=0x21a;
     assert(!pci_requester_reserve(provider,0,&key,1,&block));
     assert(amd_iommu_route_reserve(route)==-EBUSY);
     assert(!pci_requester_reserve(provider,0,&original,1,&vacant));
     pci_requester_release(vacant);pci_requester_release(block);
    }
    assert(allocated==before);
    assert(!amd_iommu_route_reserve(route)&&route->peer_count==3);
    assert(member->references>=refs+1); /* Peer pins are independent of this test's pin. */
    pci_requester_reservation_t *other=(void *)1;uint16_t key=peer_requester;
    assert(pci_requester_reserve(provider,0,&key,1,&other)==-EBUSY&&!other);
    if(strstr(name,"replaced")) {
     peer_hw->config[0]=0x99991234;pci_rescan();route_error(route,-ESTALE);
     device_t *newmember=scope_device(peer_requester);assert(newmember!=member);
     amd_iommu_route_t *fresh=NULL;assert(!amd_iommu_route_acquire(newmember,&fresh));device_put(newmember);
     assert(amd_iommu_route_reserve(fresh)==-EBUSY);
     amd_iommu_route_release(route);route=fresh;assert(!amd_iommu_route_reserve(route));
    } else if(strstr(name,"gone")) {
     peer_hw->present=0;pci_rescan();route_error(route,-ENODEV);
     assert(pci_requester_reserve(provider,0,&key,1,&other)==-EBUSY&&!other);
    } else if(strstr(name,"user-after")) {
     member->user_owned=1;route_error(route,-EBUSY);member->user_owned=0;
     assert(!amd_iommu_route_validate(route,&info));
    } else if(strstr(name,"detached")) {
     assert(!driver_unregister(&amdvi_driver));route_error(route,-ESHUTDOWN);
     assert(!amd_iommu_route_requesters(route,ids,3,&count)&&count==3);
     driver_discovery_begin();assert(!driver_register(&amdvi_driver));driver_discovery_end();
     assert(!bus_probe_device(provider));
     amd_iommu_route_t *fresh=NULL;assert(!amd_iommu_route_acquire(client,&fresh));
     assert(amd_iommu_route_reserve(fresh)==-EBUSY);
     amd_iommu_route_release(route);route=fresh;assert(!amd_iommu_route_reserve(route));
    }
   }
peer_done:
   device_put(member);
  } else if(!strncmp(name,"policy-",7)) {
   if(!strcmp(name,"policy-exclusion")||!strcmp(name,"policy-reserved"))route_policy_error(route,-EOPNOTSUPP);
   else if(!strcmp(name,"policy-invalid-unity"))route_policy_error(route,-EINVAL);
   else if(!strcmp(name,"policy-stale")) {
    controller->config[0x4c/4]=0x08080080;route_policy_error(route,-ESTALE);
   } else {
    if(!strcmp(name,"policy-allocation")) {
     for(unsigned i=1;i<=2;i++) {
      fail_at=allocations+i;route_policy_error(route,-ENOMEM);fail_at=0;
     }
    }
    struct driver_dma_policy *p=NULL;assert(!amd_iommu_route_policy_create(route,&p)&&p);
    if(!strcmp(name,"policy-deny")) {
     route_policy_check(p,0x100000,DRIVER_DMA_TO_DEVICE,-EACCES,false);
     route_policy_check(p,0x100000,DRIVER_DMA_FROM_DEVICE,-EACCES,false);
    } else if(!strcmp(name,"policy-read")||!strcmp(name,"policy-unity-read")) {
     route_policy_check(p,0x100000,DRIVER_DMA_TO_DEVICE,0,(first_flags&1)!=0);
     route_policy_check(p,0x100000,DRIVER_DMA_FROM_DEVICE,-EACCES,false);
    } else if(!strcmp(name,"policy-write")) {
     route_policy_check(p,0x100000,DRIVER_DMA_FROM_DEVICE,0,false);
     route_policy_check(p,0x100000,DRIVER_DMA_TO_DEVICE,-EACCES,false);
    } else if(!strcmp(name,"policy-linear")) {
     route_policy_check(p,0x100000,DRIVER_DMA_BIDIRECTIONAL,0,false);
    } else {
     route_policy_check(p,0x100000,DRIVER_DMA_TO_DEVICE,0,true);
     if(!strcmp(name,"policy-alias-overlap")||!strcmp(name,"policy-alternatives"))
      route_policy_check(p,0x100000,DRIVER_DMA_FROM_DEVICE,-EACCES,false);
     else route_policy_check(p,0x100000,DRIVER_DMA_FROM_DEVICE,0,true);
    }
    if(alias&&!strcmp(name,"policy-alias")) {
     route_policy_check(p,0x200000,DRIVER_DMA_FROM_DEVICE,0,true);
     route_policy_check(p,0x200000,DRIVER_DMA_TO_DEVICE,-EACCES,false);
    }
    if(!strcmp(name,"policy-detach")) {
     assert(!driver_unregister(&amdvi_driver));route_policy_error(route,-ESHUTDOWN);
     amd_iommu_route_release(route);route=NULL;assert(!route_tables);
     /* Reuse firmware storage only after all its old mappings are gone.
      * The policy must still describe the old copied restrictions. */
     route_ivrs[route_memory_offset(0)+1]=0;route_checksum(route_ivrs,route_length);
     route_policy_check(p,0x100000,DRIVER_DMA_TO_DEVICE,0,true);
     driver_discovery_begin();assert(!driver_register(&amdvi_driver));driver_discovery_end();
     assert(!bus_probe_device(provider));
    }
    route_policy_check(p,0x400000,DRIVER_DMA_BIDIRECTIONAL,0,false);
    driver_dma_policy_put(p);
   }
  } else if(!strncmp(name,"reserve-",8)) {
   if(!strcmp(name,"reserve-allocation")) {
    int n=allocated;fail_at=allocations+1;
    assert(amd_iommu_route_reserve(route)==-ENOMEM&&allocated==n);fail_at=0;
    assert(!amd_iommu_route_validate(route,&info)&&!amd_iommu_route_reserve(route));
   } else if(!strcmp(name,"reserve-user")||!strcmp(name,"reserve-alias-user")) {
    device_t *owned=alias?route->dma_client:client;owned->user_owned=1;
    assert(amd_iommu_route_reserve(route)==-EBUSY);owned->user_owned=0;
    assert(!amd_iommu_route_reserve(route));
   } else if(!strcmp(name,"reserve-bme")||!strcmp(name,"reserve-alias-bme")) {
    struct scope_function *active=alias?&scope_hw[2][4][0]:&scope_hw[2][3][2];
    active->config[1]|=4;assert(amd_iommu_route_reserve(route)==-EBUSY);
    active->config[1]&=~4U;assert(!amd_iommu_route_reserve(route));
   } else if(!strcmp(name,"reserve-read")||!strcmp(name,"reserve-alias-read")) {
    struct scope_function *bad=alias?&scope_hw[2][4][0]:&scope_hw[2][3][2];
    uint32_t command=bad->config[1];bad->config[1]=0xffffffff;
    assert(amd_iommu_route_reserve(route)==-EIO);bad->config[1]=command;
    assert(!amd_iommu_route_reserve(route));
   } else if(!strcmp(name,"reserve-generic")) {
    uint16_t keys[]={info.dma_requester};pci_requester_reservation_t *block=NULL,*free_key=NULL;
    assert(!pci_requester_reserve(provider,0,keys,1,&block));
    assert(amd_iommu_route_reserve(route)==-EBUSY);pci_requester_release(block);
    assert(!pci_requester_reserve(provider,0,keys,1,&free_key));pci_requester_release(free_key);
    assert(!amd_iommu_route_reserve(route));
   } else {
    assert(!amd_iommu_route_reserve(route));
    assert(amd_iommu_route_reserve(route)==-EALREADY);
    device_t *other_client=device_get(client);
    if(!strcmp(name,"reserve-alias")||!strcmp(name,"reserve-alias-sibling")) {
     device_put(other_client);other_client=scope_device(strstr(name,"sibling")?0x228:0x220);
    } else if(!strcmp(name,"reserve-replacement")) {
     scope_hw[2][3][2].config[0]=0x99991234;pci_rescan();route_error(route,-ENODEV);
     device_put(other_client);other_client=scope_device(0x21a);assert(other_client!=client);
    } else if(!strcmp(name,"reserve-detach")) {
     assert(!driver_unregister(&amdvi_driver));route_error(route,-ESHUTDOWN);
     assert(amd_iommu_route_reserve(route)==-ESHUTDOWN);
     driver_discovery_begin();assert(!driver_register(&amdvi_driver));driver_discovery_end();
     assert(!bus_probe_device(provider));
    } else assert(!strcmp(name,"reserve-duplicate"));
    amd_iommu_route_t *second=NULL;assert(!amd_iommu_route_acquire(other_client,&second));
    device_put(other_client);assert(amd_iommu_route_reserve(second)==-EBUSY);
    amd_iommu_route_release(route);route=second;assert(!amd_iommu_route_reserve(route));
   }
  } else if(!strcmp(name,"changed-unit")) {
   controller->config[0x4c/4]=0x08080080;route_error(route,-ESTALE);
  } else if(!strcmp(name,"changed-control")) {
   mmio[0x18]=1;route_error(route,-ESTALE);mmio[0x18]=0;
  } else if(!strcmp(name,"removed-client")) {
   device_unregister(client);route_error(route,-ENODEV);
  } else if(!strcmp(name,"replacement")) {
   scope_hw[2][3][2].config[0]=0x99991234;pci_rescan();route_error(route,-ENODEV);
   device_t *newclient=scope_device(0x21a);assert(newclient!=client);
   amd_iommu_route_t *newroute=NULL;assert(!amd_iommu_route_acquire(newclient,&newroute));
   amd_iommu_route_release(newroute);device_put(newclient);
  } else if(!strcmp(name,"range-anchor")) {
   root->config[0]=0x99991234;pci_rescan();route_error(route,-ESTALE);
  } else if(!strcmp(name,"alias-replacement")) {
   scope_hw[2][4][0].config[0]=0x99991234;pci_rescan();route_error(route,-ESTALE);
  } else if(!strcmp(name,"controller-removed")) {
   controller->present=0;pci_rescan();route_error(route,-ESHUTDOWN);
   assert(route_tables==1&&!route_unmaps);route_hw_map_check();
  } else if(!strcmp(name,"detach")) {
   amd_iommu_route_t *second=NULL;assert(!amd_iommu_route_acquire(client,&second));
   struct amdvi_owner *old=provider->drv_priv;
   assert(!driver_unregister(&amdvi_driver)&&!provider->drv&&old->detached&&old->routes==2);
   route_error(route,-ESHUTDOWN);route_acquire_error(client,-ENODEV);
   assert(!amd_iommu_route_memory(route,0,&memory)&&memory.start==0x100000);
   assert(route_tables==1&&!route_unmaps);route_hw_map_check();
   amd_iommu_route_release(second);assert(route_tables==1&&!route_unmaps);
   driver_discovery_begin();assert(!driver_register(&amdvi_driver));driver_discovery_end();
   assert(!bus_probe_device(provider)&&provider->drv_priv!=old&&route_tables==2);
   route_error(route,-ESHUTDOWN);
   amd_iommu_route_t *fresh=NULL;assert(!amd_iommu_route_acquire(client,&fresh));
   amd_iommu_route_release(route);route=fresh;
   assert(route_tables==1&&route_unmaps==1&&!amd_iommu_route_validate(route,&info));
  }
  amd_iommu_route_release(route);
 }
 assert(!driver_unregister(&amdvi_driver)&&!provider->drv&&!provider->drv_priv);
 assert(!amdvi_owners&&!route_tables&&route_maps==route_unmaps);
 if(strcmp(name,"removed-client")&&strcmp(name,"replacement")&&strcmp(name,"reserve-replacement"))assert(client->references==client_refs);
cleanup:
 device_put(client);device_put(provider);memset(scope_hw,0,sizeof(scope_hw));pci_rescan();
 assert(!g_device_count&&route_peer.references==1);
 assert(!pci_requester_reservations);
 for(unsigned i=0;i<PCI_MAX_PUBLISHED_DEVICES;i++)assert(g_pci_devs[i].references<=1);
}
'''


class AmdIommuRouteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86_64 hardware-access headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-amdvi-route-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        first = C_FIXTURE.index('uint32_t arch_pci_config_read32(')
        last = C_FIXTURE.index('static uint8_t mmio[4096]', first)
        source = C_FIXTURE[:first] + HARDWARE + C_FIXTURE[last:]
        source = source.replace('static uint8_t sizing[6];', '')
        source = source.replace('static uint8_t mmio[4096]', 'static uint8_t mmio[0x4000]')
        old = 'void *arch_device_map_mmio(uint64_t pa,size_t bytes,uint32_t flags) {(void)flags;assert(pa==0x10000000&&bytes==4096);return mmio;}\nvoid arch_device_unmap_mmio(void *p,size_t bytes) {assert(p==mmio&&bytes==4096);}'
        assert source.count(old) == 1
        source = source.replace(old, 'static void *route_map(uint64_t,size_t,uint32_t);\nstatic void route_unmap(void *,size_t);\nvoid *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t f){return route_map(pa,n,f);}\nvoid arch_device_unmap_mmio(void *p,size_t n){route_unmap(p,n);}')
        marker = 'int main(int argc,char **argv) {'
        cases = CASES
        for suffix in ('acquire', 'validate', 'memory', 'release', 'reserve', 'policy_create', 'requesters'):
            cases = cases.replace('amd_iommu_route_' + suffix + '(', 'checked_' + suffix + '(')
        source = source.replace(marker, SCOPE_CASES.split('static void scope_expect(')[0] + WRAPPERS + cases + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))', 'if(!strncmp(argv[1],"route-",6))route_case(argv[1]+6);\n else if(!strcmp(argv[1],"pci-retired"))')
        path = work / 'fixture.c'
        path.write_text(source)
        cls.binary = work / 'test'
        subprocess.run([
            'cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
            '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-DCONFIG_X86_64', '-DCONFIG_ACPI', '-DCONFIG_NR_CPUS=1',
            '-DCONFIG_DRIVER_LIFECYCLE_TEST',
            '-I' + str(ROOT / 'kernel/arch/x86_64/include'),
            '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'kernel'),
            str(path), str(ROOT / 'kernel/drivers/core/amd_iommu_probe.c'),
            str(ROOT / 'kernel/drivers/core/amd_iommu_scope.c'),
            str(ROOT / 'kernel/drivers/core/dma_policy.c'),
            str(ROOT / 'kernel/firmware/acpi/tables.c'),
            str(ROOT / 'kernel/firmware/acpi/ivrs.c'),
            str(ROOT / 'kernel/firmware/acpi/ivrs_policy.c'), '-o', str(cls.binary),
        ], check=True)

    def run_cases(self, *names):
        for name in names:
            with self.subTest(case=name):
                subprocess.run([str(self.binary), 'route-' + name], check=True, timeout=10)

    def test_firmware_topology_and_optional_hardware_range(self):
        self.run_cases('firmware', 'range')

    def test_alias_paths_and_both_ivmd_sources(self):
        self.run_cases('alias', 'alias-range')

    def test_arguments_and_allocation_rollback(self):
        self.run_cases('arguments', 'allocation')

    def test_unbound_provider_and_missing_alias_target(self):
        self.run_cases('unbound', 'alias-missing')

    def test_changed_or_insufficient_hardware_scope(self):
        self.run_cases('range-missing', 'range-root-type', 'alias-range-outside', 'changed-unit', 'changed-control')

    def test_ambiguous_firmware_ownership_cannot_select_first_unit(self):
        self.run_cases('ambiguous')

    def test_live_client_registration_and_identity(self):
        self.run_cases('removed-client', 'replacement')

    def test_anchor_and_alias_replacement_do_not_refresh_old_handles(self):
        self.run_cases('range-anchor', 'alias-replacement')

    def test_last_handle_retains_aperture_and_firmware_across_rebind(self):
        self.run_cases('detach', 'controller-removed')

    def test_explicit_reservation_and_global_pci_namespace(self):
        self.run_cases('reserve-duplicate', 'reserve-generic')

    def test_alias_target_and_sibling_alias_cannot_claim_separate_contexts(self):
        self.run_cases('reserve-alias', 'reserve-alias-sibling')

    def test_reservation_rejects_user_ownership_and_bus_mastering(self):
        self.run_cases('reserve-user', 'reserve-alias-user', 'reserve-bme', 'reserve-alias-bme', 'reserve-read', 'reserve-alias-read')

    def test_reservation_allocation_failure_keeps_observation_valid(self):
        self.run_cases('reserve-allocation')

    def test_reservation_survives_bdf_replacement_and_controller_rebind(self):
        self.run_cases('reserve-replacement', 'reserve-detach')

    def test_ivmd_access_identity_and_alias_intersections(self):
        self.run_cases('policy-rw', 'policy-read', 'policy-unity-read', 'policy-write', 'policy-linear', 'policy-deny', 'policy-alias', 'policy-alias-overlap', 'policy-alternatives')

    def test_unsupported_ivmd_cannot_become_unrestricted_policy(self):
        self.run_cases('policy-exclusion', 'policy-reserved', 'policy-invalid-unity')

    def test_policy_copy_failures_liveness_and_independent_storage(self):
        self.run_cases('policy-allocation', 'policy-stale', 'policy-detach')

    def test_complete_peer_query_and_retained_metadata(self):
        self.run_cases('peers-alias-query', 'peers-alias-detached')

    def test_every_peer_must_be_published_stopped_owned_and_covered(self):
        self.run_cases('peers-alias-missing', 'peers-alias-bme', 'peers-alias-user-before', 'peers-alias-read', 'peers-alias-range-outside')

    def test_reserved_peer_identities_survive_removal_and_replacement(self):
        self.run_cases('peers-alias-replaced', 'peers-alias-gone', 'peers-alias-user-after')

    def test_peer_set_reservation_is_atomic_and_failure_safe(self):
        self.run_cases('peers-alias-conflict', 'peers-alias-allocation')

    def test_policy_contains_sibling_peer_constraints(self):
        self.run_cases('peers-alias-policy', 'peers-alias-policy-exclusion')


if __name__ == '__main__':
    unittest.main()
