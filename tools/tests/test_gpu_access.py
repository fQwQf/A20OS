"""Production GPU policy/class leases and fbdev/sysfs access under removal/promotion."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_firmware_model import ROOT, STUBS
from .test_drv_dma_phys_host import extract_function


class GpuAccess(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-gpu-access-')
        cls.addClassCleanup(cls.temp.cleanup);work=Path(cls.temp.name)
        stubs={**STUBS,'core/arch.h':'#define PAGE_OFFSET 0x100000000UL\n','core/lock.h':'''#pragma once
#include <assert.h>
#include <pthread.h>
#include "core/types.h"
typedef struct {pthread_mutex_t native;} spinlock_t;
#define SPINLOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
static inline void spin_init(spinlock_t *p){assert(!pthread_mutex_init(&p->native,NULL));}
static inline uint64_t spin_lock_irqsave(spinlock_t *p){assert(!pthread_mutex_lock(&p->native));return 0;}
static inline void spin_unlock_irqrestore(spinlock_t *p,uint64_t f){(void)f;assert(!pthread_mutex_unlock(&p->native));}
''','core/sync.h':'''#pragma once
#include "core/lock.h"
typedef pthread_mutex_t mutex_t;
typedef struct {void *head;} wait_queue_t;
extern _Thread_local int watch_mutex;
extern int mutex_waiting;
#define MUTEX_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
static inline void mutex_init(mutex_t *m){pthread_mutexattr_t a;assert(!pthread_mutexattr_init(&a));assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));assert(!pthread_mutex_init(m,&a));assert(!pthread_mutexattr_destroy(&a));}
static inline void mutex_lock(mutex_t *m){if(watch_mutex)__atomic_store_n(&mutex_waiting,1,__ATOMIC_RELEASE);assert(!pthread_mutex_lock(m));}
static inline void mutex_unlock(mutex_t *m){assert(!pthread_mutex_unlock(m));}
static inline int mutex_trylock(mutex_t *m){return !pthread_mutex_trylock(m);}
''','core/stdio.h':'''#pragma once
#include "core/types.h"
int printf(const char *,...);int snprintf(char *,size_t,const char *,...);
''','net/socket/socket_internal.h':'#include "core/types.h"\nvoid netlink_uevent_emit(const char *,const char *,const char *,uint64_t);\n'}
        for name,body in stubs.items():
            p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
        fb=(ROOT/'kernel/drivers/gpu/framebuffer.c').read_text()
        bodies='\n'.join(extract_function(fb,name) for name in ['static void fb_fill_var_screeninfo(',
            'static int fb_apply_layout(', 'static void fb_autoflush_once(',
            'static int64_t fbdev_linux_mmap_active(', 'int64_t fbdev_linux_mmap(',
            'static int fb_ioctl_active(', 'static int fb_ioctl('])
        sysfs=(ROOT/'kernel/fs/sysfs.c').read_text()
        enum=sysfs[sysfs.index('typedef enum {'):sysfs.index('} sf_type_t;')+12]
        start=sysfs.index('/* Full state for open file handles */')
        shape=sysfs[start:sysfs.index('} sysfs_priv_t;',start)+15]
        bodies+='\n'+enum+'\n'+shape+'\n#define LOOP_MAJOR 7\n'
        bodies+='\n'.join(extract_function(sysfs,name) for name in ['static int sysfs_gpu_names_for_device(',
            'static int sysfs_gpu_names(', 'static void sysfs_display_size(', 'static sysfs_priv_t *sysfs_priv_create('])
        for file,prefix in [('simplefb.c','simplefb'),('vmsvga.c','vmsvga'),('lsdc_boot.c','lsdc_boot')]:
            bodies+='\n'+extract_function((ROOT/'kernel/drivers/gpu'/file).read_text(),'static int '+prefix+'_get_capabilities(')
        model=(ROOT/'tools/tests/gpu_access_host.c').read_text().replace('/* PRODUCTION_CONSUMERS */',bodies)
        fixture=work/'fixture.c';fixture.write_text(model);cls.binary=work/'test'
        subprocess.run(['cc','-std=gnu11','-D_GNU_SOURCE','-DCONFIG_NOMMU','-pthread','-O1','-g','-Wall','-Wextra','-Werror',
            '-Wno-misleading-indentation','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(work),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),str(fixture),
            str(ROOT/'kernel/drivers/core/driver_core.c'),str(ROOT / 'kernel/drivers/core/driver_class.c'), str(ROOT / 'kernel/drivers/core/driver_protocol.c'),
            str(ROOT/'kernel/firmware/node.c'),'-o',str(cls.binary)],check=True)

    def run_case(self,name): subprocess.run([str(self.binary),name],check=True,timeout=15)
    def test_empty_invalid_and_unregistered_admission(self):self.run_case('arguments')
    def test_physical_mapping_owner_blocks_unbind_until_last_view(self):self.run_case('physical-owner')
    def test_captured_selection_pins_device_until_class_lookup_finishes(self):self.run_case('captured-device')
    def test_policy_mutation_waits_for_lifecycle_and_revalidates_membership(self):self.run_case('mutation')
    def test_promotion_keeps_old_publication_and_exact_reference_balance(self):self.run_case('selection')
    def test_reelection_passes_offline_holes_and_sixteen_devices(self):self.run_case('election')
    def test_teardown_waits_for_call_and_offline_reader_never_waits_lifecycle(self):self.run_case('drain')
    def test_old_publication_cannot_follow_same_device_rebinding(self):self.run_case('rebind')
    def test_selection_pins_metadata_without_retaining_a_dead_binding(self):self.run_case('metadata-pin')
    def test_refresh_uses_one_binding_across_default_promotion(self):self.run_case('promotion-refresh')
    def test_sysfs_snapshot_keeps_one_identity_across_default_promotion(self):self.run_case('promotion-sysfs')
    def test_fb_ioctl_and_mmap_returns_release_calls(self):self.run_case('fb')
    def test_sysfs_gpu_attributes_and_early_exits_release_calls(self):self.run_case('sysfs')

    def test_capability_arguments_clear_outputs(self):self.run_case('cap-arguments')
    def test_provider_errors_preserve_selection_refs_and_clear_partial_results(self):self.run_case('cap-errors')
    def test_unknown_flags_and_role_dependencies_are_rejected(self):self.run_case('cap-validation')
    def test_native_roles_replace_backend_vtable_inference(self):self.run_case('cap-policy')
    def test_headless_rendering_never_takes_the_default_scanout(self):self.run_case('cap-headless')
    def test_reelection_skips_failed_capability_queries_and_releases_calls(self):self.run_case('cap-election-error')
    def test_real_firmware_svga_and_lsdc_providers_declare_only_implemented_roles(self):self.run_case('cap-providers')

    def test_failed_initial_capability_probe_has_no_publication_or_selection_pin(self):self.run_case('cap-probe-error')

    def test_managed_scanout_replaces_firmware_and_election_uses_all_priority_levels(self):self.run_case('cap-firmware-priority')
