"""Production Rust VMO snapshot and C mapping adapter, with resize/invalidation races."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_firmware_model import ROOT, STUBS
from .test_drv_dma_phys_host import extract_function


class DriverDmaVmo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-dma-vmo-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        stubs = {**STUBS, 'core/arch.h': '#define PAGE_OFFSET 0x100000000UL\n',
                 'core/lock.h': '''#pragma once
#include <assert.h>
#include <pthread.h>
#include "core/types.h"
typedef struct {pthread_mutex_t native;} spinlock_t;
#define SPINLOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
static inline void spin_init(spinlock_t *l){assert(!pthread_mutex_init(&l->native,NULL));}
static inline uint64_t spin_lock_irqsave(spinlock_t *l){assert(!pthread_mutex_lock(&l->native));return 0;}
static inline void spin_unlock_irqrestore(spinlock_t *l,uint64_t f){(void)f;assert(!pthread_mutex_unlock(&l->native));}
''', 'core/sync.h': '''#pragma once
#include "core/lock.h"
typedef pthread_mutex_t mutex_t;
typedef struct {void *head;} wait_queue_t;
static inline void mutex_init(mutex_t *l){assert(!pthread_mutex_init(l,NULL));}
static inline void mutex_lock(mutex_t *l){assert(!pthread_mutex_lock(l));}
static inline void mutex_unlock(mutex_t *l){assert(!pthread_mutex_unlock(l));}
''', 'drivers/core/driver_hwapi.h': '''#pragma once
#include "core/types.h"
struct device;
uint64_t dma_get_mask(const struct device *);
void dma_sync_for_device(void *,size_t);
void dma_sync_for_cpu(void *,size_t);
'''}
        for name, content in stubs.items():
            path = work/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content)
        owner = work/'owner.rs'
        owner.write_text((ROOT/'kernel/mm/vmo_lifecycle.rs').read_text()+'\nextern crate std;\n')
        library = work/'owner.a'
        subprocess.run(['rustc','--edition=2021','--crate-type=staticlib','-Cpanic=abort','-Copt-level=1',str(owner),'-o',str(library)],check=True)
        production=(ROOT/'kernel/mm/vmo.c').read_text()
        names=['int vmo_pin_pages(', 'uint32_t a20_vmo_lifecycle_type_locked(',
               'uint32_t a20_vmo_lifecycle_pin_limit(',
               'int a20_vmo_lifecycle_frame_valid(', 'void a20_vmo_lifecycle_pin_frames(',
               'void a20_vmo_lifecycle_ref_inc(', 'int a20_vmo_lifecycle_ref_dec_last(',
               'int a20_vmo_lifecycle_lock(', 'void a20_vmo_lifecycle_unlock(',
               'uint32_t a20_vmo_lifecycle_page_count_locked(', 'pfn_t a20_vmo_lifecycle_slot_locked(']
        bodies='\n'.join(extract_function(production,name) for name in names)
        model=(ROOT/'tools/tests/driver_dma_vmo_host.c').read_text()
        model=model.replace('/* PRODUCTION_VMO_PRIMITIVES */','extern int a20_vmo_pin_pages_rust(struct vmo *,uint32_t,uint32_t,pfn_t *);\n'+bodies)
        fixture=work/'fixture.c';fixture.write_text(model);cls.binary=work/'test'
        subprocess.run(['cc','-std=gnu11','-pthread','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(work),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),
                        str(fixture),str(ROOT/'kernel/drivers/core/dma_vmo.c'),
                        str(ROOT/'kernel/drivers/core/dma_mapping.c'),str(ROOT/'kernel/drivers/core/dma_policy.c'),
                        str(ROOT/'tools/tests/dma_buffer_unused_host.c'),str(library),'-ldl','-lm','-o',str(cls.binary)],check=True)

    def run_cases(self,*names):
        for name in names:
            with self.subTest(name=name): subprocess.run([str(self.binary),name],check=True,timeout=15)

    def test_snapshot_outlives_vmo_and_exposes_selected_pages(self): self.run_cases('success')
    def test_materialized_paged_and_sparse_sources(self): self.run_cases('paged','sparse')
    def test_snapshot_blocks_resize_until_all_pins_are_owned(self): self.run_cases('race')
    def test_provider_can_resize_after_snapshot_without_a_vmo_lock(self): self.run_cases('resize')
    def test_failed_map_rollback_retains_only_mapping_pins(self): self.run_cases('rollback')
    def test_partial_unmap_retains_all_pages_until_retry(self): self.run_cases('unmap')
    def test_holes_do_not_materialize_or_publish_partial_pins(self): self.run_cases('hole')
    def test_physical_and_unknown_sources_are_rejected(self): self.run_cases('physical')
    def test_bad_arguments_and_ranges_clear_output_without_pins(self): self.run_cases('parameters')
    def test_allocation_failures_return_temporary_snapshot_pins(self): self.run_cases('oom')
    def test_policy_closure_and_architecture_failures_release_snapshot(self): self.run_cases('policy','closed','architecture')

    def test_rust_lifecycle_decisions_and_maximum_snapshot(self):
        binary=Path(self.temp.name)/'rust-tests'
        subprocess.run(['rustc','--edition=2021','--test',str(ROOT/'kernel/mm/vmo_lifecycle.rs'),'-o',str(binary)],check=True)
        subprocess.run([str(binary),'--test-threads=1'],check=True,timeout=15)
