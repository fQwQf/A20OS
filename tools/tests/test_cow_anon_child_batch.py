"""Exercise the bounded child leaf mapper and Rust COW ownership transfer.

The C mapper, page-frame geometry helpers, leaf installer, and Rust transfer
guard are extracted from the production sources under ROOT/kernel.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TREE = ROOT


def extract_block(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f"unterminated block: {signature}")


def extract_struct(source: str, signature: str) -> str:
    start = source.index(signature)
    end = source.index("};", start) + 2
    return source[start:end]


class AnonChildBatch(unittest.TestCase):
    def test_real_c_mapper_preflight_and_installation(self):
        source = (TREE / "kernel/mm/mm.c").read_text()
        fixture = Path(__file__).with_name("anon_child_mapper_host.c").read_text()
        frame = (TREE / "kernel/include/mm/frame.h").read_text()
        conversions = "\n\n".join(extract_block(frame, signature) for signature in [
            "static inline const pfa_range_t *pfa_range_for_pfn(",
            "static inline const pfa_range_t *pfa_range_for_pa(",
            "static inline void *pfn_to_virt(",
            "static inline paddr_t pfn_to_phys(",
            "static inline pfn_t phys_to_pfn(",
            "static inline int pfn_valid(",
        ])
        fixture = fixture.replace("/* REAL_PFA_CONVERSIONS */", conversions)
        fixture = fixture.replace(
            "/* REAL_LEAF_INSTALL */",
            extract_block(source, "static void pt_install_leaf_cls("),
        ).replace(
            "/* REAL_SCALAR_MAPPER */",
            extract_block(source, "int pt_map_cls("),
        ).replace(
            "/* REAL_BATCH_MAPPER */",
            extract_block(source, "int pt_map_anon_batch("),
        )
        header = (TREE / "kernel/include/mm/mm.h").read_text()
        assert_start = header.rindex("#if __SIZEOF_POINTER__ == 8", 0, header.index("_Static_assert(sizeof(struct a20_cow_anon_batch_page)"))
        assert_end = header.index("/* Map a bounded anonymous fork batch", assert_start)
        fixture = fixture.replace("/* ANON_BATCH_ABI_ASSERTS */", header[assert_start:assert_end])
        with tempfile.TemporaryDirectory() as tmp:
            cfile, exe = Path(tmp) / "mapper.c", Path(tmp) / "mapper"
            cfile.write_text(fixture)
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_linear_pfn_to_pa_negative_control_fails_expected_pa(self):
        source = (TREE / "kernel/mm/mm.c").read_text()
        old = "paddr_t pa = pfn_to_phys((pfn_t)pages[i].pfn);"
        self.assertEqual(source.count(old), 1)
        # Reconstruct only the old faulty line in the extracted production
        # function; the fixture checks known range-base addresses independently.
        fixture = Path(__file__).with_name("anon_child_mapper_host.c").read_text()
        frame = (TREE / "kernel/include/mm/frame.h").read_text()
        conversions = "\n\n".join(extract_block(frame, signature) for signature in [
            "static inline const pfa_range_t *pfa_range_for_pfn(",
            "static inline const pfa_range_t *pfa_range_for_pa(",
            "static inline void *pfn_to_virt(",
            "static inline paddr_t pfn_to_phys(",
            "static inline pfn_t phys_to_pfn(",
            "static inline int pfn_valid(",
        ])
        fixture = fixture.replace("/* REAL_PFA_CONVERSIONS */", conversions)
        fixture = fixture.replace("/* REAL_LEAF_INSTALL */", extract_block(source, "static void pt_install_leaf_cls("))
        fixture = fixture.replace("/* REAL_SCALAR_MAPPER */", extract_block(source, "int pt_map_cls("))
        old_mapper = source.replace(old, "paddr_t pa = PFN2PA(pages[i].pfn);")
        fixture = fixture.replace("/* REAL_BATCH_MAPPER */", extract_block(old_mapper, "int pt_map_anon_batch("))
        # The intentionally wrong physical addresses no longer resolve through
        # the synthetic PFA, so executable-cache maintenance is skipped in this
        # negative control; the positive fixture retains the full 64-flush check.
        flush_assert = "assert(walk_calls==1 && installs==64 && status_calls==64 && exec_flushes==64);"
        self.assertEqual(fixture.count(flush_assert), 1)
        fixture = fixture.replace(flush_assert,
                                  "assert(walk_calls==1 && installs==64 && status_calls==64 && exec_flushes==0);")
        header = (TREE / "kernel/include/mm/mm.h").read_text()
        assert_start = header.rindex("#if __SIZEOF_POINTER__ == 8", 0, header.index("_Static_assert(sizeof(struct a20_cow_anon_batch_page)"))
        assert_end = header.index("/* Map a bounded anonymous fork batch", assert_start)
        fixture = fixture.replace("/* ANON_BATCH_ABI_ASSERTS */", header[assert_start:assert_end])
        with tempfile.TemporaryDirectory() as tmp:
            cfile, exe = Path(tmp) / "negative.c", Path(tmp) / "negative"
            cfile.write_text(fixture)
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(result.returncode, 77, result.stderr.decode(errors="replace"))
            self.assertIn(b"EXPECTED_PA_MISMATCH", result.stderr)

    def test_real_rust_guard_transfers_or_releases_whole_batch(self):
        source = (TREE / "kernel/mm/cow_clone.rs").read_text()
        start = source.index("#[repr(C)]\n#[derive(Clone, Copy, Default)]\nstruct AnonBatchPage")
        end = source.index("\n/// Clone one present leaf", start)
        code = source[start:end]
        code = code.replace(extract_block(code, 'unsafe extern "C" {'), "")
        harness = r'''use core::ffi::c_void;
const ANON_BATCH_PAGES: usize = 64;
const PAGE_SIZE: usize = 4096;
const ENOMEM: i32 = -12;
unsafe extern "C" {
    #[link_name="mock_private"] fn a20_cow_requires_private_copy() -> i32;
    #[link_name="mock_snapshot"] fn a20_cow_share_anon_batch_snapshot(
        parent:*mut c_void, child:*mut c_void, start:usize, node:*mut c_void,
        out:*mut AnonBatchPage) -> i32;
    #[link_name="mock_map_batch"] fn a20_cow_map_child_anon_batch(
        child:*mut c_void, va:usize, pages:*const AnonBatchPage, count:usize) -> i32;
    #[link_name="mock_put_many"] fn frame_put_many(pfns:*const u32, count:usize);
    #[link_name="mock_rss"] fn a20_cow_rss_add(child:*mut c_void, pages:usize);
}
/* ACTUAL_RUST_BATCH_CODE */
static mut PRIVATE_COPY:i32=0;
static mut SNAPSHOT_RESULT:i32=1;
static mut MAP_RESULT:i32=0;
static mut SNAPSHOTS:usize=0;
static mut MAP_CALLS:usize=0;
static mut PINS:usize=0;
static mut RELEASED:usize=0;
static mut CHILD_REFS:usize=0;
static mut RSS:usize=0;
#[no_mangle] unsafe extern "C" fn mock_private()->i32 { unsafe{PRIVATE_COPY} }
#[no_mangle] unsafe extern "C" fn mock_snapshot(_p:*mut c_void,_c:*mut c_void,_v:usize,_n:*mut c_void,
                                                   out:*mut AnonBatchPage)->i32 {
    unsafe { SNAPSHOTS+=1; if SNAPSHOT_RESULT!=1 { return SNAPSHOT_RESULT; }
        PINS=64; for i in 0..64 { *out.add(i)=AnonBatchPage{pfn:(i+1) as u32,class:2,flags:0x205}; }
        1 }
}
#[no_mangle] unsafe extern "C" fn mock_map_batch(_c:*mut c_void,va:usize,
                                                  pages:*const AnonBatchPage,count:usize)->i32 {
    unsafe { MAP_CALLS+=1; assert_eq!(va,0x20000); assert_eq!(count,64);
        for i in 0..count { let p=*pages.add(i); assert_eq!(p.pfn,(i+1) as u32); assert_eq!(p.class,2); assert_eq!(p.flags,0x205); }
        if MAP_RESULT==0 { PINS-=64; CHILD_REFS+=64; }
        MAP_RESULT }
}
#[no_mangle] unsafe extern "C" fn mock_put_many(pfns:*const u32,count:usize) {
    unsafe { for i in 0..count { assert!(*pfns.add(i)!=0); }
        assert_eq!(PINS,count); PINS-=count; RELEASED+=count; }
}
#[no_mangle] unsafe extern "C" fn mock_rss(_c:*mut c_void,pages:usize) { unsafe { RSS+=pages; } }
unsafe fn reset() { unsafe { PRIVATE_COPY=0; SNAPSHOT_RESULT=1; MAP_RESULT=0;
    SNAPSHOTS=0; MAP_CALLS=0; PINS=0; RELEASED=0; CHILD_REFS=0; RSS=0; } }
fn main() { unsafe {
    reset(); PRIVATE_COPY=1;
    assert_eq!(a20_cow_clone_anon_batch_rust(core::ptr::null_mut(),core::ptr::null_mut(),0x20000,core::ptr::null_mut()),0);
    assert_eq!((SNAPSHOTS,MAP_CALLS),(0,0));
    reset(); SNAPSHOT_RESULT=0;
    assert_eq!(a20_cow_clone_anon_batch_rust(core::ptr::null_mut(),core::ptr::null_mut(),0x20000,core::ptr::null_mut()),0);
    assert_eq!((PINS,RELEASED,MAP_CALLS,RSS),(0,0,0,0));
    reset();
    assert_eq!(a20_cow_clone_anon_batch_rust(core::ptr::null_mut(),core::ptr::null_mut(),0x20000,core::ptr::null_mut()),1);
    assert_eq!((PINS,RELEASED,CHILD_REFS,MAP_CALLS,RSS),(0,0,64,1,64));
    reset(); MAP_RESULT=ENOMEM;
    assert_eq!(a20_cow_clone_anon_batch_rust(core::ptr::null_mut(),core::ptr::null_mut(),0x20000,core::ptr::null_mut()),ENOMEM);
    assert_eq!((PINS,RELEASED,CHILD_REFS,MAP_CALLS,RSS),(0,64,0,1,0));
    reset(); SNAPSHOT_RESULT=-5;
    assert_eq!(a20_cow_clone_anon_batch_rust(core::ptr::null_mut(),core::ptr::null_mut(),0x20000,core::ptr::null_mut()),-5);
    assert_eq!((PINS,RELEASED,CHILD_REFS,MAP_CALLS,RSS),(0,0,0,0,0));
} }
'''.replace("/* ACTUAL_RUST_BATCH_CODE */", code)
        with tempfile.TemporaryDirectory() as tmp:
            rs, exe = Path(tmp) / "batch.rs", Path(tmp) / "batch"
            rs.write_text(harness)
            subprocess.run(["rustc", "--edition=2021", "-C", "panic=abort", str(rs), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
