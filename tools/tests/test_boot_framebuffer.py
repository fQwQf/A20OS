"""Exercise production GOP decoding and common scanout geometry validation."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class Framebuffer(C.Structure):
    _fields_ = [('magic', C.c_uint32), ('version', C.c_uint32),
                ('physical', C.c_uint64), ('aperture', C.c_uint64),
                *[(name, C.c_uint32) for name in ('width', 'height', 'stride', 'bpp',
                                                'red', 'green', 'blue', 'reserved')]]

class BootFramebuffer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-gop-')
        work = Path(cls.temp.name)
        (work / 'wrapper.c').write_text('''
#define KERNEL_MEMORY_BYTES 33554432ULL
#define KERNEL_LOAD_ADDRESS 0x2000000ULL
#include "kernel/arch/x86_64/boot/uefi/x86_64_loader.c"
const uint8_t _binary_kernel_bin_start[1]={0};
const uint8_t _binary_kernel_bin_end[1]={0};
int decode(uint32_t format,uint32_t pitch,uint32_t width,uint32_t height,
           uint64_t base,uint64_t size,uint32_t r,uint32_t g,uint32_t b,
           uint32_t reserved,boot_framebuffer_t *out){
 struct efi_gop_info info={0,width,height,format,r,g,b,reserved,pitch};
 struct efi_gop_mode mode={1,0,&info,sizeof(info),base,size};
 return describe_gop(&mode,out);
}
int missing_info(void){boot_framebuffer_t fb;return describe_gop(NULL,&fb);}
''')
        subprocess.run(['cc', '-shared', '-fPIC', '-O1', '-fshort-wchar', '-I', str(ROOT),
                        '-I', str(ROOT / 'kernel/include'), str(work/'wrapper.c'),
                        str(ROOT/'kernel/boot/framebuffer.c'), '-o', str(work/'test.so')],
                       check=True, capture_output=True)
        cls.lib = C.CDLL(str(work/'test.so'))
        cls.lib.decode.argtypes = [C.c_uint32]*4 + [C.c_uint64]*2 + [C.c_uint32]*4 + [C.POINTER(Framebuffer)]
        cls.lib.boot_framebuffer_validate.argtypes = [C.POINTER(Framebuffer)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def decode(self, *, fmt=1, pitch=1056, width=1024, height=768,
               base=0x80000000, size=8*1024*1024, masks=(0,0,0,0)):
        fb = Framebuffer()
        rc = self.lib.decode(fmt,pitch,width,height,base,size,*masks,C.byref(fb))
        return rc, fb

    def test_rgb_bgr_and_padding(self):
        for fmt, red, blue in [(0,0xff,0xff0000),(1,0xff0000,0xff)]:
            rc, fb = self.decode(fmt=fmt)
            self.assertEqual(rc,0)
            self.assertEqual((fb.stride,fb.red,fb.blue),(4224,red,blue))
            self.assertEqual(self.lib.boot_framebuffer_validate(C.byref(fb)),0)

    def test_rgb565_and_packed24(self):
        for masks,bpp in [((0xf800,0x7e0,0x1f,0),16),((0xff0000,0xff00,0xff,0),24)]:
            rc,fb = self.decode(fmt=2,masks=masks)
            self.assertEqual(rc,0)
            self.assertEqual(fb.bpp,bpp)
            self.assertEqual(self.lib.boot_framebuffer_validate(C.byref(fb)),0)

    def test_blt_only_and_invalid_masks(self):
        for fmt,masks in [(3,(0,0,0,0)),(4,(0,0,0,0)),
                          (2,(0,0xff00,0xff,0)),(2,(0xff,0xff,0xff0000,0)),
                          (2,(0x55,0xff00,0xff0000,0)),(2,(0xff,0xff00,0xff0000,0x80))]:
            self.assertNotEqual(self.decode(fmt=fmt,masks=masks)[0],0)
        self.assertNotEqual(self.lib.missing_info(),0)

    def test_geometry_and_address_failures(self):
        for values in [dict(pitch=1023),dict(width=0),dict(height=0),dict(width=20000),
                       dict(base=0),dict(base=0x80000001),dict(base=0xfffffffffffff000),
                       dict(base=0x2000000),dict(size=4096),dict(size=512*1024*1024)]:
            with self.subTest(values=values):
                self.assertNotEqual(self.decode(**values)[0],0)

    def test_framebuffer_above_4g_is_preserved_for_late_mapping(self):
        rc, fb = self.decode(base=0x100000000)
        self.assertEqual(rc, 0)
        self.assertEqual(fb.physical, 0x100000000)

    def test_common_validator_rejects_corrupt_handoff(self):
        for field,value in [('magic',0),('version',2),('physical',1),('aperture',4096),
                            ('stride',1),('bpp',8),('red',0),('blue',0xff00)]:
            rc,fb=self.decode()
            self.assertEqual(rc,0)
            setattr(fb,field,value)
            self.assertNotEqual(self.lib.boot_framebuffer_validate(C.byref(fb)),0)

if __name__ == '__main__':
    unittest.main()
