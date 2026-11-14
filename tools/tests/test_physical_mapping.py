"""Native physical mapping policy plus production MM bridges/fbdev/udriver."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_drv_dma_phys_host import extract_function

ROOT=Path(__file__).resolve().parents[2]

def build_fixture(work, policy_text=None, unmap_transform=None, narrow_pa=False, narrow_va=False):
    obj=work/'policy.o'
    policy=work/'policy.rs'
    policy.write_text(policy_text if policy_text is not None else (ROOT/'kernel/mm/mmap_policy.rs').read_text())
    subprocess.run(['rustc','--edition=2021','--crate-type','lib','--emit=obj','-C','panic=abort',
                    '-C','opt-level=2',str(policy),'-o',str(obj)],check=True)
    mm=(ROOT/'kernel/mm/mm.c').read_text()
    pt='\n'.join(extract_function(mm,n) for n in ['static int pt_entry_mapped(',
        'static int pt_table_empty(pte_t *table, int level, int cleared_idx) {',
        'int pt_unmap(mm_struct_t *mm, vaddr_t va)'])
    if unmap_transform: pt=unmap_transform(pt)
    mmap=(ROOT/'kernel/mm/mmap.c').read_text()
    spec=mmap[mmap.index('struct a20_mmap_seg_spec {'):mmap.index('};',mmap.index('struct a20_mmap_seg_spec {'))+2]
    bridges='\n'.join(extract_function(mmap,n) for n in [
        'int a20_mmap_physical_supported(', 'pte_t a20_mmap_physical_pte(',
        'uint64_t a20_mmap_physical_enter(', 'void a20_mmap_physical_finish(',
        'int a20_mmap_physical_occupied(', 'int a20_mmap_physical_install(',
        'void a20_mmap_physical_unmap(', 'void a20_mmap_physical_note(',
        'void a20_mmap_physical_rss(', 'vaddr_t mm_mmap_physical(', 'void a20_mmap_seg_fill('])
    flags=(ROOT/'kernel/mm/pte_flags.c').read_text()
    flags='\n'.join(extract_function(flags,n) for n in ['pte_t mm_prot_to_pte_flags(', 'int mm_pte_flags_to_prot(', 'pte_t mm_pte_flags_apply_prot('])
    body=spec+';\n'+flags+'\nextern uintptr_t a20_mmap_physical_rust(void *,uintptr_t,size_t,int,int,uint64_t,uintptr_t);\n'+bridges
    driver=extract_function((ROOT/'kernel/drivers/core/udriver.c').read_text(),'int udriver_map_mmio(')
    fb=(ROOT/'kernel/drivers/gpu/framebuffer.c').read_text()
    fb_body='\n'.join(extract_function(fb,n) for n in ['static int64_t fb_map_scanout(', 'static int64_t fb_map_physical(', 'static int64_t fbdev_linux_mmap_active('])
    ioctl=extract_function(fb,'static int fb_ioctl_active(')
    branch=ioctl[ioctl.index('        case FBIO_MAP_FB: {'):ioctl.index('        case FBIO_FLUSH:')]
    fb_body+='\nstatic int fb_ioctl_map(struct gpu_device_access *access,void *arg){\nconst gpu_dev_ops_t *ops=access->ops;struct device *dev=access->device;switch(FBIO_MAP_FB){\n'+branch+'\n}return -EINVAL;}\n'
    source=(ROOT/'tools/tests/physical_mapping_host.c').read_text()
    for marker,text in [('PT',pt),('MM',body),('UDRIVER',driver),('FB',fb_body)]:
        source=source.replace('/* PRODUCTION_'+marker+' */',text)
    if narrow_pa: source=source.replace('typedef uintptr_t paddr_t;', 'typedef uint32_t paddr_t;')
    if narrow_va: source=source.replace('typedef uintptr_t vaddr_t;', 'typedef uint32_t vaddr_t;')
    fixture=work/'fixture.c';fixture.write_text(source);binary=work/'test'
    subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all',str(fixture),str(obj),'-o',str(binary)],check=True)
    return binary


class PhysicalMapping(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-physical-map-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary=build_fixture(Path(cls.temp.name))

    def run_case(self,name):subprocess.run([str(self.binary),name],check=True,timeout=15)
    def test_rejects_invalid_ranges_permissions_and_placement(self):self.run_case('arguments')
    def test_eager_layout_cache_permissions_ceiling_and_accounting(self):self.run_case('layout')
    def test_protection_preserves_device_cache_type_and_dirty_history(self):self.run_case('protection')
    def test_full_hint_overlap_noreplace_and_hint_fallback(self):self.run_case('placement')
    def test_fixed_uses_locked_unmap_and_propagates_failure(self):self.run_case('fixed')
    def test_orphan_physical_zero_leaf_is_not_overwritten(self):self.run_case('orphan')
    def test_metadata_oom_before_any_transaction_or_install(self):self.run_case('oom')
    def test_each_pt_allocation_boundary_prunes_failed_page_and_prefix(self):self.run_case('rollback')
    def test_actual_fbdev_mmu_and_udriver_consumers(self):self.run_case('consumers')

    def test_rollback_controls_fail_at_runtime(self):
        source=(ROOT/'kernel/mm/mmap_policy.rs').read_text()
        no_current=source.replace('a20_mmap_physical_unmap(mm, va + off);','let _ = mm;')
        no_prefix=source.replace('a20_mmap_physical_unmap(mm, va + done);','let _ = done;')
        def no_prune(text):
            old='if (!(pte & PTE_V)) break;'
            assert old in text
            return text.replace(old,'if (!(pte & PTE_V)) return -EINVAL;')
        for name,policy,transform in [('failed-page',no_current,None),('prefix',no_prefix,None),('empty-prune',source,no_prune)]:
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix='a20-physical-control-') as tmp:
                binary=build_fixture(Path(tmp),policy,transform)
                result=subprocess.run([str(binary),'rollback'],capture_output=True,text=True,timeout=15)
                self.assertNotEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertRegex(result.stderr,r'Assertion|ERROR: AddressSanitizer|runtime error:')

    def test_physical_width_before_install(self):
        self.run_case('address-width')
        with tempfile.TemporaryDirectory(prefix='a20-physical-pa32-') as tmp:
            binary=build_fixture(Path(tmp),narrow_pa=True)
            subprocess.run([str(binary),'address-width'],check=True,timeout=15)

    def test_fb_adapter_keeps_narrow_va_errors_negative(self):
        with tempfile.TemporaryDirectory(prefix='a20-physical-va32-') as tmp:
            binary=build_fixture(Path(tmp),narrow_va=True)
            subprocess.run([str(binary),'consumers'],check=True,timeout=15)
