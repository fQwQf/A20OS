"""Production DT simple-framebuffer parsing, selection and pre-PFA reservation."""
import ctypes as C
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from raspberrypi4_fixture import Tree

class DtFramebuffer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-dtfb-');work=Path(cls.temp.name)
        (work/'wrap.c').write_text('''
#include "firmware/devicetree.h"
static fw_dt_t tree;
int load(const void *data,size_t bytes){return fw_dt_init(&tree,data,bytes);}
int select_fb(uint64_t *out){boot_framebuffer_t fb;enum boot_framebuffer_memory memory;fw_node_t node;
 int rc=fw_dt_framebuffer_select(&tree,&fb,&memory,&node);if(!rc){out[0]=fb.magic;out[1]=fb.physical;out[2]=fb.aperture_bytes;out[3]=fb.width;out[4]=fb.height;out[5]=fb.stride;out[6]=fb.bits_per_pixel;out[7]=memory;out[8]=fb.red_mask;out[9]=fb.blue_mask;}return rc;}
int reservations(unsigned cap,uint64_t *out){fw_dt_range_t ram[16],held[16];fw_dt_memory_t map={ram,held,16,cap,0,0};int rc=fw_dt_memory(&tree,0x4f000000,&map);out[0]=map.reserved_count;if(!rc)for(size_t i=0;i<map.reserved_count;i++){out[1+i*2]=held[i].base;out[2+i*2]=held[i].size;}return rc;}
int window(uint64_t base,uint64_t end){fw_dt_range_t usable[16];size_t n;return fw_dt_memory_window(&tree,0x4f000000,0x40000000,0x50000000,base,end,usable,16,&n,0);}
int resource(const char *path,uint64_t *out){fw_node_t root,node;fw_resource_t range;int rc=fw_dt_root(&tree,&root);if(!rc)rc=fw_dt_find_path(&root,path,&node);if(!rc)rc=node.ops->resource(&node,0,&range);if(!rc){out[0]=range.address;out[1]=range.size;}return rc;}
''')
        sources=['kernel/core/fdt_view.c','kernel/firmware/devicetree/node.c',
                 'kernel/firmware/devicetree/memory.c','kernel/firmware/devicetree/framebuffer.c',
                 'kernel/boot/framebuffer.c']
        subprocess.run(['cc','-shared','-fPIC','-O2','-std=gnu99','-fno-builtin',
            '-Wall','-Wextra','-Werror','-I'+str(ROOT/'kernel/include'),str(work/'wrap.c'),
            *[str(ROOT/source) for source in sources],'-o',str(work/'test.so')],check=True,capture_output=True)
        cls.lib=C.CDLL(str(work/'test.so'));cls.lib.load.argtypes=[C.c_void_p,C.c_size_t]
        cls.lib.window.argtypes=[C.c_uint64,C.c_uint64]
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def build(self, *, count=1, fmt='x8r8g8b8', stride=1280, width=320,height=240,
              address=0x47000000,size=0x100000,disabled=False,trim=False,prefer=None,
              display0=None,region=False,bad_region=False,malformed=False):
        t=Tree();t.begin('');t.cells('#address-cells',2);t.cells('#size-cells',2)
        t.begin('memory@40000000');t.text('device_type','memory');t.cells('reg',0,0x40000000,0,0x7000000 if trim else 0x10000000);t.end()
        if region:
            t.begin('reserved-memory');t.cells('#address-cells',2);t.cells('#size-cells',2);t.prop('ranges',b'')
            t.begin('scanout@47000000');t.cells('phandle',99);t.cells('reg',0,address,0,size);t.end();t.end()
        if display0:
            t.begin('aliases');t.text('display0',display0);t.end()
        t.begin('chosen')
        if prefer:t.text('stdout-path',prefer)
        for i in range(count):
            t.begin('framebuffer@%x'%(address+i*0x100000));t.text('compatible','simple-framebuffer')
            if disabled:t.text('status','disabled')
            if region:t.cells('memory-region',99 if not bad_region else 98)
            else:t.cells('reg',0,address+i*0x100000,0,size)
            t.cells('width',width);t.cells('height',height);t.cells('stride',stride)
            if malformed:t.prop('format',fmt.encode())
            else:t.text('format',fmt)
            t.end()
        t.end();t.end();return t.finish([])
    def load(self,**kw):
        blob=self.build(**kw);self.buffer=C.create_string_buffer(blob)
        self.assertEqual(self.lib.load(self.buffer,len(blob)),0)
    def select(self):
        out=(C.c_uint64*40)();rc=self.lib.select_fb(out);return rc,list(out)
    def test_selected_ram_and_canonical_resource(self):
        self.load();rc,out=self.select();self.assertEqual(rc,0)
        self.assertEqual(out[1:8],[0x47000000,0x100000,320,240,1280,32,1])
        value=(C.c_uint64*2)();self.assertEqual(self.lib.resource(b'/chosen/framebuffer@47000000',value),0)
        self.assertEqual(list(value),[0x47000000,0x100000])
    def test_trimmed_uboot_ram_requires_architecture_classification(self):
        self.load(trim=True);rc,out=self.select();self.assertEqual(rc,0);self.assertEqual(out[7],2)
    def test_disabled_does_not_publish_or_reserve(self):
        self.load(disabled=True);rc,out=self.select();self.assertEqual(rc,0);self.assertEqual(out[0],0)
        held=(C.c_uint64*40)();self.assertEqual(self.lib.reservations(16,held),0);self.assertEqual(held[0],1)
    def test_all_active_scanouts_reserved(self):
        self.load(count=2);held=(C.c_uint64*40)();self.assertEqual(self.lib.reservations(16,held),0)
        self.assertEqual(list(held)[1:3],[0x47000000,0x200000])
        self.assertEqual(self.select()[0],-17)
    def test_stdout_and_alias_selection(self):
        for hint in ['stdout','alias']:
            args={'count':2,'prefer':'/chosen/framebuffer@47100000'} if hint=='stdout' else {'count':2,'display0':'/chosen/framebuffer@47100000'}
            self.load(**args);rc,out=self.select();self.assertEqual(rc,0);self.assertEqual(out[1],0x47100000)
    def test_memory_region_binding(self):
        self.load(region=True);self.assertEqual(self.select()[0],0)
        value=(C.c_uint64*2)();self.assertEqual(self.lib.resource(b'/chosen/framebuffer@47000000',value),0)
        self.assertEqual(list(value),[0x47000000,0x100000])
        self.load(region=True,bad_region=True);self.assertNotEqual(self.select()[0],0)
    def test_invalid_format_and_geometry(self):
        for args in [dict(stride=1279),dict(stride=4),
                     dict(width=0),dict(height=0),dict(address=0x47000001),dict(size=4096),
                     dict(malformed=True)]:
            with self.subTest(args=args):self.load(**args);self.assertNotEqual(self.select()[0],0)
    def test_unsupported_mode_retains_extent_and_continues_serial(self):
        for fmt in ['unknown','a8r8g8b8']:
            self.load(fmt=fmt);rc,out=self.select();self.assertEqual(rc,0);self.assertEqual(out[0],0)
            held=(C.c_uint64*40)();self.assertEqual(self.lib.reservations(16,held),0)
            self.assertEqual(list(held)[1:3],[0x47000000,0x100000])
            self.assertNotEqual(self.lib.window(0x47000000,0x47001000),0)
    def test_rgb565_bgr_and_24bit(self):
        for fmt,stride,bits,red,blue in [('r5g6b5',640,16,0xf800,0x1f),
                ('x8b8g8r8',1280,32,0xff,0xff0000),('r8g8b8',960,24,0xff0000,0xff)]:
            self.load(fmt=fmt,stride=stride);rc,out=self.select();self.assertEqual(rc,0)
            self.assertEqual([out[6],out[8],out[9]],[bits,red,blue])
    def test_capacity_failure_and_kernel_overlap(self):
        self.load();held=(C.c_uint64*40)();self.assertEqual(self.lib.reservations(0,held),-28)
        self.assertEqual(held[0],0)
        self.assertNotEqual(self.lib.window(0x47000000,0x47001000),0)
        self.assertEqual(self.lib.window(0x40080000,0x41000000),0)
    def test_partial_ram_overlap_rejected(self):
        self.load(address=0x4ff00000,size=0x200000);self.assertNotEqual(self.select()[0],0)

if __name__=='__main__':unittest.main()
