"""Opt-in RK3566 firmware provenance and actual scanout handoff contracts."""
from pathlib import Path
import json
import sys
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import quartz64_zero as quartz


class QuartzVideoContracts(unittest.TestCase):
    def test_nested_a20_make_variables_do_not_override_uboot_arch(self):
        from unittest.mock import patch
        import os
        with patch.dict(os.environ, {"ARCH": "aarch64", "MAKEFLAGS": "ARCH=aarch64 -j8",
                                    "MAKEOVERRIDES": "ARCH=aarch64", "MAKELEVEL": "2",
                                    "MFLAGS": "-j8", "PATH": "/compiler/bin"}, clear=True):
            env = quartz.uboot_environment()
        self.assertEqual(env["PATH"], "/compiler/bin")
        self.assertEqual(env["SOURCE_DATE_EPOCH"], quartz.SOURCE_DATE_EPOCH)
        self.assertEqual(env["TZ"], "UTC")
        for key in ("ARCH", "MAKEFLAGS", "MAKEOVERRIDES", "MAKELEVEL", "MFLAGS"):
            self.assertNotIn(key, env)

    def test_series_is_complete_and_digest_pinned(self):
        rows = json.loads((quartz.VIDEO_FILES / "series.json").read_text())
        self.assertEqual(len(rows), 9)
        for n, row in enumerate(rows, 1):
            self.assertIn(f"[{n}/9]", row["subject"])
            self.assertEqual(quartz.sha256(quartz.VIDEO_FILES / "patches" / row["file"]),
                             row["sha256"])
        self.assertNotEqual(quartz.VIDEO_SOURCE["commit"], quartz.SOURCES["u-boot"]["commit"])

    def test_zero_power_and_graph_are_explicit(self):
        dts = (quartz.VIDEO_FILES / "rk3566-quartz64-zero-video.dtsi").read_text()
        self.assertIn("zero_hdmi_0v9: LDO_REG2", dts)
        self.assertIn("zero_hdmi_1v8: LDO_REG7", dts)
        for name in ("zero_hdmi_con", "zero_hdmi_out", "zero_hdmi_in", "zero_vp0_hdmi"):
            self.assertIn(name + ":", dts)
            self.assertIn("<&" + name + ">", dts)
        self.assertNotIn("&hdmi_sound", dts)

    def test_handoff_uses_live_mode_and_full_reservation(self):
        glue = (quartz.VIDEO_FILES / "a20_simplefb.c").read_text()
        self.assertIn("if (!video_is_active())", glue)
        self.assertIn("fdt_add_fb_mem_rsv(fdt)", glue)
        self.assertIn('fdt_path_offset(fdt, "/chosen")', glue)
        self.assertIn("fdt_setup_simplefb_node(fdt, node, plat->base, video->xsize", glue)
        self.assertIn("video->ysize, video->line_length, format)", glue)
        self.assertIn("EVENT_SPY_FULL(EVT_FT_FIXUP", glue)
        self.assertIn("# CONFIG_VIDEO_REMOVE is not set", quartz.VIDEO_CONFIG)

class QuartzHandoffRuntime(unittest.TestCase):
    def test_live_mode_and_headless_paths_execute_production_glue(self):
        import shutil
        import subprocess
        import tempfile
        if not shutil.which("cc"):
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "linux").mkdir()
            (stage / "stub.h").write_text(r'''
#ifndef STUB_H
#define STUB_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t u64;
struct udevice { int unused; };
struct video_priv { unsigned xsize, ysize, bpix, line_length, format; };
struct video_uc_plat { unsigned long base, size; };
struct event { struct { struct { struct { void *fdt; } tree; } ft_fixup; } data; };
#define VIDEO_BPP32 5
#define VIDEO_X8R8G8B8 3
#define VIDEO_X8B8G8R8 2
#define UCLASS_VIDEO 1
#define EVT_FT_FIXUP 1
#define EVENT_SPY_FULL(a,b)
int video_is_active(void);
int uclass_first_device_err(int, struct udevice **);
void *dev_get_uclass_priv(struct udevice *);
void *dev_get_uclass_plat(struct udevice *);
int fdt_add_fb_mem_rsv(void *);
int fdt_path_offset(void *, const char *);
int fdt_add_subnode(void *, int, const char *);
int fdt_setprop_u32(void *, int, const char *, unsigned);
int fdt_setprop(void *, int, const char *, const void *, int);
int fdt_setprop_string(void *, int, const char *, const char *);
int fdt_setup_simplefb_node(void *, int, u64, unsigned, unsigned, unsigned, const char *);
#endif
''')
            for name in ("dm.h", "event.h", "fdt_support.h", "video.h", "linux/libfdt.h"):
                (stage / name).write_text('#include "stub.h"\n')
            source = r'''
#include "stub.h"
#include <assert.h>
#include <string.h>
static int active, reserved, published;
static struct udevice dev;
static struct video_priv mode = {1280,720,5,5120,3};
static struct video_uc_plat allocation = {0x3d000000,0x2000000};
int video_is_active(void) { return active; }
int uclass_first_device_err(int id, struct udevice **out) { *out=&dev; return 0; }
void *dev_get_uclass_priv(struct udevice *d) { return &mode; }
void *dev_get_uclass_plat(struct udevice *d) { return &allocation; }
int fdt_add_fb_mem_rsv(void *f) { reserved++; return 0; }
int fdt_path_offset(void *f, const char *p) { assert(!strcmp(p,"/chosen")); return 1; }
int fdt_add_subnode(void *f,int n,const char *s) { assert(n==1); return 2; }
int fdt_setprop_u32(void *f,int n,const char *s,unsigned v) { assert(v==2); return 0; }
int fdt_setprop(void *f,int n,const char *s,const void *p,int l) { return 0; }
int fdt_setprop_string(void *f,int n,const char *s,const char *v) { return 0; }
int fdt_setup_simplefb_node(void *f,int n,u64 base,unsigned w,unsigned h,unsigned stride,const char *fmt) {
 assert(reserved==1 && n==2 && base==allocation.base);
 assert(w==1280 && h==720 && stride==5120 && !strcmp(fmt,"x8r8g8b8"));
 published++;return 0;
}
#include "GLUE"
int main(void) {
 struct event event={0};
 assert(!zero_simplefb_fixup(0,&event) && !reserved && !published);
 active=1; mode.format=99;
 assert(zero_simplefb_fixup(0,&event)==-EINVAL && !published);
 mode.format=3; mode.line_length=4096;
 assert(zero_simplefb_fixup(0,&event)==-EINVAL && !reserved);
 mode.line_length=5120; allocation.size=100;
 assert(zero_simplefb_fixup(0,&event)==-EINVAL && !reserved);
 allocation.size=0x2000000;
 assert(!zero_simplefb_fixup(0,&event) && reserved==1 && published==1);
 return 0;
}
'''.replace("GLUE", str(quartz.VIDEO_FILES / "a20_simplefb.c"))
            (stage / "test.c").write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", str(stage),
                            str(stage / "test.c"), "-o", str(stage / "test")], check=True,
                           capture_output=True, text=True)
            subprocess.run([str(stage / "test")], check=True, capture_output=True)

class QuartzOpaqueRuntime(unittest.TestCase):
    def test_bottom_mixer_forces_opaque_independent_of_pixel_alpha(self):
        import subprocess
        import tempfile
        source = r'''
#include <assert.h>
#include <stdint.h>
static uint32_t registers[0x800/4];
static void writel(uint32_t value, void *address) { *(uint32_t *)address=value; }
#include "HEADER"
int main(void) {
 a20_vop2_opaque(registers);
 unsigned src=registers[0x640/4], dst=registers[0x644/4];
 assert(src==0x00ff01a1 && dst==0x00ff0060);
 assert(((src>>2)&3)==0); /* GLOBAL, not per-pixel alpha */
 assert(((src>>5)&7)==5 && ((src>>16)&255)==255);
 assert(((dst>>5)&7)==3); /* inverse source alpha => destination factor zero */
 assert(registers[0x648/4]==0x20 && registers[0x64c/4]==0x78);
 assert(!registers[0x6c0/4]); /* HDR bypass */
 for(unsigned pixel_alpha=0;pixel_alpha<256;pixel_alpha++) {
  (void)pixel_alpha;
  unsigned global_alpha=(src>>16)&255;
  for(unsigned color=0;color<256;color++)
   assert((color*global_alpha + 193*(255-global_alpha))/255==color);
 }
 return 0;
}
'''.replace("HEADER", str(quartz.VIDEO_FILES / "a20_vop2_opaque.h"))
        with tempfile.TemporaryDirectory() as directory:
            stage=Path(directory)
            (stage / "opaque.c").write_text(source)
            build=subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(stage/"opaque.c"),
                                  "-o", str(stage/"opaque")], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            subprocess.run([str(stage/"opaque")], check=True)


if __name__ == "__main__":
    unittest.main()
