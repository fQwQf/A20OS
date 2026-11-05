"""Production allocation-free DeviceTree provider semantics on host fixtures."""
from pathlib import Path
import ctypes
import struct
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "boards"))
from raspberrypi4_fixture import Tree

class Provider(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name)
  (p/'wrap.c').write_text('''
#include "firmware/devicetree.h"
static fw_dt_t tree;
int load(const void*b,size_t n){return fw_dt_init(&tree,b,n);}
int resource(const char*path,unsigned index,uint64_t*out){fw_node_t root,node;fw_resource_t r;int rc=fw_dt_root(&tree,&root);if(!rc)rc=fw_dt_find_path(&root,path,&node);if(!rc)rc=node.ops->resource(&node,index,&r);if(!rc){out[0]=r.address;out[1]=r.size;}return rc;}
int available(const char*path){fw_node_t r,n;int rc=fw_dt_root(&tree,&r);if(!rc)rc=fw_dt_find_path(&r,path,&n);return rc?rc:n.ops->available(&n);}
int irq(const char*path,unsigned index,uint32_t*out){fw_node_t r,n;fw_reference_t ref;int rc=fw_dt_root(&tree,&r);if(!rc)rc=fw_dt_find_path(&r,path,&n);if(!rc)rc=n.ops->irq(&n,index,&ref);if(!rc){out[0]=ref.count;for(size_t i=0;i<ref.count;i++)out[i+1]=ref.args[i];}return rc;}
int args(char*out,size_t n){return fw_dt_bootargs(&tree,out,n);}
int root_valid(void){fw_node_t r;return fw_dt_root(&tree,&r);}
int phandle(unsigned id){fw_node_t r,n;int rc=fw_dt_root(&tree,&r);return rc?rc:fw_dt_find_phandle(&r,id,&n);}
int reference(const char*path,unsigned index,uint32_t*out){fw_node_t r,n;fw_reference_t ref;int rc=fw_dt_root(&tree,&r);if(!rc)rc=fw_dt_find_path(&r,path,&n);if(!rc)rc=n.ops->reference(&n,"clocks","#clock-cells",index,&ref);if(!rc){out[0]=ref.count;for(size_t i=0;i<ref.count;i++)out[i+1]=ref.args[i];}return rc;}
int named_resource(const char*path,unsigned index,char*out){fw_node_t r,n;fw_resource_t res;int rc=fw_dt_root(&tree,&r);if(!rc)rc=fw_dt_find_path(&r,path,&n);if(!rc)rc=n.ops->resource(&n,index,&res);if(!rc){size_t i=0;if(res.name)for(;res.name[i]&&i<63;i++)out[i]=res.name[i];out[i]=0;}return rc;}
int window(unsigned flags,unsigned capacity,uint64_t*out){fw_dt_range_t usable[16];size_t count=0;int rc=fw_dt_memory_window(&tree,0x100000,0x80000000,0x90000000,0x80200000,0x81000000,usable,capacity,&count,flags);out[0]=count;if(!rc)for(size_t i=0;i<count;i++){out[i*2+1]=usable[i].base;out[i*2+2]=usable[i].size;}return rc;}
int memory(unsigned cap,uint64_t*out){fw_dt_range_t ram[16],reserved[16],usable[16];fw_dt_memory_t m={ram,reserved,cap,cap,0,0};int rc=fw_dt_memory(&tree,0x100000,&m);size_t n=0;if(!rc)rc=fw_dt_memory_subtract(&m,usable,cap,&n);if(!rc){out[0]=n;for(size_t i=0;i<n;i++){out[i*2+1]=usable[i].base;out[i*2+2]=usable[i].size;}}return rc;}
''')
  subprocess.run(['cc','-shared','-fPIC','-O2','-std=gnu99','-fno-builtin','-Wall','-Wextra','-Werror','-I',str(ROOT/'kernel/include'),str(p/'wrap.c'),str(ROOT/'kernel/core/fdt_view.c'),str(ROOT/'kernel/firmware/devicetree/node.c'),str(ROOT/'kernel/firmware/devicetree/memory.c'), str(ROOT/'kernel/firmware/devicetree/framebuffer.c'), str(ROOT/'kernel/boot/framebuffer.c'),'-o',str(p/'dt.so')],check=True)
  cls.lib=ctypes.CDLL(str(p/'dt.so'));cls.lib.load.argtypes=[ctypes.c_void_p,ctypes.c_size_t]
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def load(self,blob):
  self.blob=ctypes.create_string_buffer(blob);return self.lib.load(self.blob,len(blob))
 def tree(self,*,ranges=True,disabled=False,extended=False,bad=False):
  t=Tree();t.begin('');t.cells('#address-cells',2);t.cells('#size-cells',2)
  t.begin('memory@80000000');t.text('device_type','memory');t.cells('reg',0,0x80000000,0,0x10000,0,0x90000000,0,0x10000);t.end()
  t.begin('reserved-memory');t.cells('#address-cells',2);t.cells('#size-cells',2);t.prop('ranges',b'')
  t.begin('hole@80004000');t.cells('reg',0,0x80004000,0,0x2000);t.end();t.end()
  t.begin('chosen');t.text('bootargs','a20.test=1');t.end()
  t.begin('interrupt-controller');t.cells('phandle',1);t.cells('#interrupt-cells',2);t.prop('interrupt-controller',b'');t.end()
  t.begin('bus');t.cells('#address-cells',1);t.cells('#size-cells',1);t.cells('interrupt-parent',1)
  if disabled:t.text('status','disabled')
  if ranges:t.cells('ranges',0,0,0x40000000,0x10000)
  t.begin('uart@20');t.cells('reg',0x20,0x10,0x80,0x20)
  if extended:t.cells('interrupts-extended',1,9,4,1,10,1)
  else:t.cells('interrupts',9,4,10,1)
  t.end();t.end();t.end();return t.finish([])
 def test_translation_multiple_regs_and_parent_cells(self):
  self.assertEqual(self.load(self.tree()),0);v=(ctypes.c_uint64*2)()
  self.assertEqual(self.lib.resource(b'/bus/uart@20',0,v),0);self.assertEqual(list(v),[0x40000020,0x10])
  self.assertEqual(self.lib.resource(b'/bus/uart@20',1,v),0);self.assertEqual(list(v),[0x40000080,0x20])
  self.assertEqual(self.lib.resource(b'/bus/uart@20',2,v),-2)
 def test_disabled_bus_and_missing_ranges(self):
  self.assertEqual(self.load(self.tree(disabled=True)),0);self.assertEqual(self.lib.available(b'/bus/uart@20'),0)
  self.assertEqual(self.load(self.tree(ranges=False)),0);v=(ctypes.c_uint64*2)();self.assertEqual(self.lib.resource(b'/bus/uart@20',0,v),-95)
 def test_inherited_and_extended_interrupts(self):
  for extended in [False,True]:
   self.assertEqual(self.load(self.tree(extended=extended)),0);v=(ctypes.c_uint32*17)();self.assertEqual(self.lib.irq(b'/bus/uart@20',1,v),0);self.assertEqual(list(v)[:3],[2,10,1])
 def test_multiple_ram_and_internal_reservation_are_not_prefix_clipped(self):
  self.assertEqual(self.load(self.tree()),0);v=(ctypes.c_uint64*33)();self.assertEqual(self.lib.memory(16,v),0);self.assertEqual(list(v)[:7],[3,0x80000000,0x4000,0x80006000,0xa000,0x90000000,0x10000])
  self.assertEqual(self.lib.memory(1,v),-28)
 def test_bootargs_bound_and_malformed_blob(self):
  blob=self.tree();self.assertEqual(self.load(blob),0);out=ctypes.create_string_buffer(64);self.assertEqual(self.lib.args(out,64),0);self.assertEqual(out.value,b'a20.test=1');self.assertEqual(self.lib.args(out,4),-28);self.assertEqual(out.value,b'')
  self.assertEqual(self.load(blob[:-1]),-22)
  self.assertEqual(self.lib.root_valid(),-22)
 def test_nested_bus_translation_and_named_resources(self):
  t=Tree();t.begin('');t.cells('#address-cells',2);t.cells('#size-cells',1)
  t.begin('outer');t.cells('#address-cells',1);t.cells('#size-cells',1);t.cells('ranges',0x100,0,0x40000000,0x1000)
  t.begin('inner');t.cells('#address-cells',1);t.cells('#size-cells',1);t.cells('ranges',0,0x100,0x100)
  t.begin('uart');t.cells('reg',0x20,0x10);t.text('reg-names','control');t.end();t.end();t.end();t.end()
  self.assertEqual(self.load(t.finish([])),0);v=(ctypes.c_uint64*2)()
  self.assertEqual(self.lib.resource(b'/outer/inner/uart',0,v),0);self.assertEqual(list(v),[0x40000020,0x10])
  name=ctypes.create_string_buffer(64);self.assertEqual(self.lib.named_resource(b'/outer/inner/uart',0,name),0);self.assertEqual(name.value,b'control')
 def test_empty_ranges_identity_and_unmapped_is_an_error(self):
  for mapped in [True,False]:
   t=Tree();t.begin('');t.cells('#address-cells',1);t.cells('#size-cells',1)
   t.begin('bus');t.cells('#address-cells',1);t.cells('#size-cells',1)
   if mapped:t.prop('ranges',b'')
   else:t.cells('ranges',0,0x1000,0x10)
   t.begin('dev');t.cells('reg',0x20,0x10);t.end();t.end();t.end()
   self.assertEqual(self.load(t.finish([])),0);v=(ctypes.c_uint64*2)()
   self.assertEqual(self.lib.resource(b'/bus/dev',0,v),0 if mapped else -34)
   if mapped:self.assertEqual(list(v),[0x20,0x10])
 def test_reference_holes_and_malformed_trailing_entry(self):
  for malformed in [False,True]:
   t=Tree();t.begin('');t.begin('clock');t.cells('phandle',1);t.cells('#clock-cells',1);t.end()
   t.begin('dev');t.cells('clocks',0,1,7,*([1] if malformed else []));t.end();t.end()
   self.assertEqual(self.load(t.finish([])),0);v=(ctypes.c_uint32*17)()
   self.assertEqual(self.lib.reference(b'/dev',0,v),-22 if malformed else -2)
   self.assertEqual(self.lib.reference(b'/dev',1,v),-22 if malformed else 0)
   if not malformed:self.assertEqual(list(v)[:2],[1,7])
 def test_duplicate_phandles_and_invalid_status(self):
  t=Tree();t.begin('');t.begin('a');t.cells('phandle',3);t.end();t.begin('b');t.cells('phandle',3);t.prop('status',b'okay');t.end();t.end()
  self.assertEqual(self.load(t.finish([])),0);self.assertEqual(self.lib.phandle(3),-22);self.assertEqual(self.lib.available(b'/b'),-22)
 def test_explicit_static_ram_policy_still_honors_reservations(self):
  t=Tree();t.begin('');t.cells('#address-cells',1);t.cells('#size-cells',1)
  t.begin('reserved-memory');t.cells('#address-cells',1);t.cells('#size-cells',1);t.prop('ranges',b'')
  t.begin('hole');t.cells('reg',0x84000000,0x1000);t.end();t.end();t.end()
  self.assertEqual(self.load(t.finish([])),0);v=(ctypes.c_uint64*33)()
  self.assertEqual(self.lib.window(0,16,v),-2)
  self.assertEqual(self.lib.window(1,16,v),0);self.assertEqual(list(v)[:5],[2,0x80000000,0x4000000,0x84001000,0xbfff000])
  self.assertEqual(self.lib.window(1,1,v),-28);self.assertEqual(v[0],0)
  self.assertEqual(self.lib.window(2,16,v),-22)
 def test_malformed_memory_type_cannot_trigger_static_fallback(self):
  t=Tree();t.begin('');t.cells('#address-cells',1);t.cells('#size-cells',1)
  t.begin('memory');t.prop('device_type',b'memory');t.cells('reg',0x80000000,0x10000000);t.end();t.end()
  self.assertEqual(self.load(t.finish([])),0);v=(ctypes.c_uint64*33)();self.assertEqual(self.lib.window(1,16,v),-22)
 def interrupt_map_tree(self,*,extended=False,mask=True,missing_reg=False,device_address=0x1900,
                        malformed_mask=False,tail=(),duplicate=False,unknown=False,cycle=False,
                        passthru=False,controller_cells=3,disabled=False,empty=False):
  t=Tree();t.begin('');t.cells('#address-cells',2);t.cells('#size-cells',2)
  t.begin('gic');t.cells('phandle',1);t.cells('#address-cells',0);t.cells('#interrupt-cells',controller_cells);t.prop('interrupt-controller',b'')
  if disabled:t.text('status','disabled')
  t.end()
  t.begin('plic');t.cells('phandle',2);t.cells('#address-cells',0);t.cells('#interrupt-cells',1);t.prop('interrupt-controller',b'');t.end()
  t.begin('outer');t.cells('phandle',4);t.cells('#address-cells',1);t.cells('#interrupt-cells',2)
  if cycle:t.cells('interrupt-map',0x55,9,4,3,0x1800,0,0,1)
  else:t.cells('interrupt-map',0x55,9,4,99 if unknown else 1,0,64,4)
  t.end()
  t.begin('pci');t.cells('phandle',3);t.cells('#address-cells',3);t.cells('#size-cells',2);t.cells('#interrupt-cells',1)
  if mask:t.cells('interrupt-map-mask',*([0xfffff800,0,0] if malformed_mask else [0xfffff800,0,0,7]))
  if passthru:t.cells('interrupt-map-pass-thru',0,0,0,1)
  # Distinct parent cell widths in the same table: PLIC 0+1, outer nexus 1+2.
  first=[0x2800,0,0,1,2,22]
  selected=[0x1800,0,0,1,4,0x55,9,4]
  t.cells('interrupt-map',*([] if empty else first+selected+(selected if duplicate else [])+list(tail)))
  t.begin('device')
  if not missing_reg:t.cells('reg',device_address,0,0,0,0x1000)
  if extended:t.cells('interrupts-extended',3,1)
  else:t.cells('interrupts',1)
  t.end();t.end();t.end();return t.finish([])
 def test_interrupt_map_pci_cells_mask_and_recursive_parent(self):
  for extended in [False,True]:
   self.assertEqual(self.load(self.interrupt_map_tree(extended=extended)),0);v=(ctypes.c_uint32*17)()
   self.assertEqual(self.lib.irq(b'/pci/device',0,v),0);self.assertEqual(list(v)[:4],[3,0,64,4])
  self.assertEqual(self.load(self.interrupt_map_tree(mask=False,device_address=0x1800)),0)
  self.assertEqual(self.lib.irq(b'/pci/device',0,v),0)
 def test_interrupt_map_variable_parent_entry_widths(self):
  self.assertEqual(self.load(self.interrupt_map_tree(device_address=0x2800)),0);v=(ctypes.c_uint32*17)()
  self.assertEqual(self.lib.irq(b'/pci/device',0,v),0);self.assertEqual(list(v)[:2],[1,22])
 def test_interrupt_map_missing_or_ambiguous_match_is_rejected(self):
  for settings,error in [({'device_address':0x3800},-34),({'duplicate':True},-22),({'unknown':True},-19)]:
   self.assertEqual(self.load(self.interrupt_map_tree(**settings)),0);v=(ctypes.c_uint32*17)()
   self.assertEqual(self.lib.irq(b'/pci/device',0,v),error,settings)
 def test_interrupt_map_malformed_and_capacity_inputs_are_rejected(self):
  for settings,error in [({'missing_reg':True},-22),({'malformed_mask':True},-22),
                          ({'tail':[0]},-22),({'empty':True},-22),({'controller_cells':17},-7)]:
   self.assertEqual(self.load(self.interrupt_map_tree(**settings)),0);v=(ctypes.c_uint32*17)()
   self.assertEqual(self.lib.irq(b'/pci/device',0,v),error,settings)
 def test_interrupt_map_cycle_disabled_provider_and_pass_thru(self):
  for settings,error in [({'cycle':True},-40),({'disabled':True},-19),({'passthru':True},-95)]:
   self.assertEqual(self.load(self.interrupt_map_tree(**settings)),0);v=(ctypes.c_uint32*17)()
   self.assertEqual(self.lib.irq(b'/pci/device',0,v),error,settings)
if __name__=='__main__':unittest.main()
