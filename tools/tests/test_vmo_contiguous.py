"""Native contiguous RAM construction/split + actual Rust final release policy."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_firmware_model import ROOT, STUBS
from .test_drv_dma_phys_host import extract_function

def build_fixture(work, suffix=None, extra_vmo=(), transform=None):
    stubs={**STUBS,'core/sync.h':'#pragma once\ntypedef struct {void *head;} wait_queue_t;\nstatic inline void wait_queue_init(wait_queue_t *q){q->head=0;}\n', 'core/arch.h':'extern uintptr_t host_page_offset;\n#define PAGE_OFFSET host_page_offset\n',
      'core/lock.h':'''#pragma once
#include <assert.h>
#include <pthread.h>
#include "core/types.h"
typedef struct {pthread_mutex_t native;} spinlock_t;
#define SPINLOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
static inline void spin_init(spinlock_t *p){assert(!pthread_mutex_init(&p->native,NULL));}
static inline uint64_t spin_lock_irqsave(spinlock_t *p){assert(!pthread_mutex_lock(&p->native));return 0;}
static inline void spin_unlock_irqrestore(spinlock_t *p,uint64_t flags){(void)flags;assert(!pthread_mutex_unlock(&p->native));}
'''}
    for name,body in stubs.items():
        p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
    life=work/'life.o'
    subprocess.run(['rustc','--edition=2021','--crate-type','lib','--emit=obj','-Cpanic=abort','-Copt-level=2',str(ROOT/'kernel/mm/vmo_lifecycle.rs'),'-o',str(life)],check=True)
    source=(ROOT/'kernel/mm/vmo_page.rs').read_text();start=source.index('unsafe extern "C" {');end=source.index('\n}\n',start)+3
    # Execute the exact production destruction transaction, not a model of
    # its flush/release ordering. Other page/resize transactions have their
    # existing complete host suite and are not rewritten into this fixture.
    body='#![no_std]\n#![allow(dead_code)]\nuse core::ffi::c_void;\nconst PFN_NONE:u32=u32::MAX;\n'+source[start:end]+'\n'
    body+=extract_function(source,'unsafe fn destroy(')+'\n#[no_mangle]\n'+extract_function(source,'pub unsafe extern "C" fn a20_vmo_destroy_rust(')
    rust=work/'destroy.rs';rust.write_text(body);destroy=work/'destroy.o'
    subprocess.run(['rustc','--edition=2021','--crate-type','lib','--emit=obj','-Cpanic=abort','-Copt-level=2',str(rust),'-o',str(destroy)],check=True)
    c=(ROOT/'kernel/mm/vmo.c').read_text()
    names=['struct vmo *vmo_create(', 'int vmo_create_contiguous(', 'int vmo_contiguous_info(',
      'void vmo_ref(', 'void vmo_release(', 'pfn_t vmo_peek_page(', 'int vmo_pin_pages(', 'int64_t vmo_resize(',
      'int a20_vmo_resize_lock(', 'int a20_vmo_page_lock(', 'void a20_vmo_lifecycle_ref_inc(',
      'int a20_vmo_lifecycle_ref_dec_last(', 'int a20_vmo_lifecycle_lock(', 'void a20_vmo_lifecycle_unlock(',
      'uint32_t a20_vmo_lifecycle_type_locked(', 'uint32_t a20_vmo_lifecycle_pin_limit(',
      'int a20_vmo_lifecycle_frame_valid(', 'void a20_vmo_lifecycle_pin_frames(',
      'uint32_t a20_vmo_lifecycle_page_count_locked(', 'pfn_t a20_vmo_lifecycle_slot_locked(',
      'void a20_vmo_destroy_notify(', 'void a20_vmo_destroy_detach_pager(', 'uint32_t a20_vmo_destroy_page_count(',
      'pfn_t a20_vmo_destroy_page(', 'uint64_t a20_vmo_destroy_phys_size(', 'uint64_t a20_vmo_destroy_charged_pages(',
      'void a20_vmo_destroy_stats(', 'void a20_vmo_destroy_flush_tlb(', 'void a20_vmo_destroy_release_page(',
      'void a20_vmo_destroy_uncharge(', 'void a20_vmo_destroy_free_pages(', 'void a20_vmo_destroy_free_object(']
    functions='\n'.join(extract_function(c,n) for n in [*names,*extra_vmo])
    split=extract_function((ROOT/'kernel/mm/frame.c').read_text(),'int pfa_split_allocated(')
    model=(ROOT/'tools/tests/vmo_contiguous_host.c').read_text()
    if transform is not None: model=transform(model)
    if suffix is not None: model=model[:model.index('static void *ref_worker(')]+suffix
    fixture=work/'fixture.c';fixture.write_text(model.replace('/* PRODUCTION_SPLIT */',split).replace('/* PRODUCTION_VMO */',functions));binary=work/'test'
    subprocess.run(['cc','-std=gnu11','-pthread','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
      '-I'+str(work),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),str(fixture),str(life),str(destroy),'-o',str(binary)],check=True)
    return binary


class VmoContiguous(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-contiguous-vmo-');cls.addClassCleanup(cls.temp.cleanup)
        cls.binary=build_fixture(Path(cls.temp.name))
    def run_case(self,name):subprocess.run([str(self.binary),name],check=True,timeout=15)
    def test_constructor_validation_and_factory_only_kind(self):self.run_case('parameters')
    def test_canonical_order_zero_ownership_padding_and_physical_zero(self):self.run_case('layout')
    def test_maximum_extent(self):self.run_case('maximum')
    def test_allocation_and_split_failures_leave_no_owner(self):self.run_case('oom')
    def test_extent_and_slots_are_immutable(self):self.run_case('immutable')
    def test_native_pins_survive_source_destruction(self):self.run_case('pins')
    def test_concurrent_object_references_release_storage_once(self):self.run_case('concurrent')
