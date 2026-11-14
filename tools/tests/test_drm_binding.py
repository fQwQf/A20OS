"""Compile actual DRM/core/class code; model VFS, MM, work and scheduler edges."""
from pathlib import Path
import tempfile
import subprocess
import unittest
from .test_firmware_model import ROOT, STUBS
from .test_drv_dma_phys_host import extract_function

class DrmBinding(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-drm-binding-');cls.addClassCleanup(cls.temp.cleanup)
        work=Path(cls.temp.name)
        stubs=dict(STUBS)
        for p in ['core/defs.h','mm/mm.h']:stubs.pop(p)
        stubs.update({
          'core/arch.h':'#pragma once\n#include "'+str(ROOT/'kernel/include/core/arch.h')+'"\n#undef PAGE_OFFSET\nextern uintptr_t host_page_offset;\n#define PAGE_OFFSET host_page_offset\n',
          'core/lock.h':'''#pragma once
#include <pthread.h>
#include <assert.h>
#include "core/types.h"
typedef struct {pthread_mutex_t native;} spinlock_t;
#define SPINLOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
static inline void spin_init(spinlock_t *s){assert(!pthread_mutex_init(&s->native,NULL));}
static inline uint64_t spin_lock_irqsave(spinlock_t *s){assert(!pthread_mutex_lock(&s->native));return 0;}
static inline void spin_unlock_irqrestore(spinlock_t *s,uint64_t f){(void)f;assert(!pthread_mutex_unlock(&s->native));}
''',
          'core/sync.h':'#include "'+str(ROOT/'kernel/include/core/sync.h')+'"\n',
          'drivers/core/driver_hwapi.h':'#pragma once\n#include "core/types.h"\nuint64_t clock_get_ticks(void);\nuint64_t clock_ticks_per_sec(void);\n',
          'sys/usercopy.h':'''#pragma once
#include "core/types.h"
int copy_from_user(void *,const void *,size_t);
int copy_to_user(void *,const void *,size_t);
''',
          'net/socket/socket_internal.h':'#include "core/types.h"\nvoid netlink_uevent_emit(const char *,const char *,const char *,uint64_t);\n',
        })
        for name,body in stubs.items():
            p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
        code=work/'fixture.c'
        model=(ROOT/'tools/tests/drm_binding_host.c').read_text()
        code.write_text(model.replace('/* PRODUCTION_ANONFD */',extract_function((ROOT/'kernel/fs/anonfd.c').read_text(),'int anonfd_install_vfile(')))
        cls.binary=work/'test'
        subprocess.run(['cc','-std=gnu11','-D_GNU_SOURCE','-DCONFIG_X86_64','-DCONFIG_64BIT','-DCONFIG_NR_CPUS=2','-pthread','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
          '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
          '-I'+str(work),'-I'+str(ROOT/'kernel/arch/x86_64/include'),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),
          str(code),str(ROOT/'kernel/drivers/core/driver_core.c'),str(ROOT / 'kernel/drivers/core/driver_class.c'), str(ROOT / 'kernel/drivers/core/driver_protocol.c'),str(ROOT/'kernel/firmware/node.c'),'-o',str(cls.binary)],check=True)
    def run_case(self,name):subprocess.run([str(self.binary),name],check=True,timeout=15)
    def test_initial_binding(self):self.run_case('initial')
    def test_exact_devres_release_preserves_remaining_order(self):self.run_case('actions')
    def test_default_promotion_never_changes_an_open_binding(self):self.run_case('binding')
    def test_handles_require_file_ownership_and_device_namespaces_stay_separate(self):self.run_case('namespaces')
    def test_prime_import_requires_the_original_publication(self):self.run_case('prime')
    def test_edid_cache_is_per_publication(self):self.run_case('edid')
    def test_unref_failure_retains_ram_and_retries(self):self.run_case('unref-retry')
    def test_offline_is_not_stop_and_successful_remove_retires_storage(self):self.run_case('offline-retention')
    def test_exported_dmabuf_keeps_cpu_storage_after_binding_stop(self):self.run_case('dmabuf-offline')
    def test_failed_attach_keeps_published_backing_until_stop(self):self.run_case('attach-failure')
    def test_failed_resource_create_keeps_cleanup_identity(self):self.run_case('resource-failure')
    def test_failed_context_create_is_not_ready_and_destroy_is_retryable(self):self.run_case('context-failure')
    def test_rebind_has_new_namespace_and_old_file_stays_offline(self):self.run_case('rebind')
    def test_offline_between_read_check_and_wait_link_cannot_lose_wake(self):self.run_case('read-race')
    def test_remove_drains_the_actual_inflight_drm_lease(self):self.run_case('lease-drain')
    def test_fd_install_failure_returns_the_export_pin(self):self.run_case('fd-failure')
    def test_errors_return_every_active_call(self):self.run_case('errors')
    def test_open_allocation_failures_leave_no_file_or_publication_owner(self):
        for at in range(5):
            with self.subTest(allocation=at):self.run_case('open-oom-'+str(at))

    def test_missing_remove_cannot_authorize_persistent_drm_backing(self):self.run_case('missing-remove')
    def test_mmap_reuses_native_cap_transaction_and_preserves_errors(self):self.run_case('mmap')
    def test_failed_output_copy_rolls_back_unpublished_buffers(self):self.run_case('copyout-rollback')
    def test_old_retirement_never_unrefs_reused_ids_in_new_binding(self):self.run_case('rebind-resource')

    def test_fd_lookup_install_and_last_put_never_hold_a_gpu_mutex(self):self.run_case('prime-boundaries')

    def test_file_deny(self):self.run_case("file-deny")
    def test_file_close(self):self.run_case("file-close")
    def test_file_map_close(self):self.run_case("file-map-close")
    def test_file_create_oom(self):self.run_case("file-create-oom")
    def test_file_close_failure(self):self.run_case("file-close-failure")
    def test_file_import_close(self):self.run_case("file-import-close")
    def test_file_import_idempotent(self):self.run_case("file-import-idempotent")
    def test_file_import_oom(self):self.run_case("file-import-oom")
    def test_file_import_copyout(self):self.run_case("file-import-copyout")
    def test_file_flink(self):self.run_case("file-flink")
    def test_file_flink_rollback(self):self.run_case("file-flink-rollback")
    def test_file_framebuffer(self):self.run_case("file-framebuffer")
    def test_file_getfb_rollback(self):self.run_case("file-getfb-rollback")
    def test_file_stop_entries(self):self.run_case("file-stop-entries")
    def test_file_names_expire_with_the_last_handle_even_if_export_pins_storage(self):self.run_case("file-name-expiry")
    def test_online_close_reclaims_a_mixed_batch_while_import_and_vma_survive(self):self.run_case("file-close-many")
    def test_resident_budget(self):self.run_case("resident-budget")
    def test_resident_errors(self):self.run_case("resident-errors")
    def test_resident_capture(self):self.run_case("resident-capture")
    def test_resident_offline(self):self.run_case("resident-offline")
    def test_resident_retained(self):self.run_case("resident-retained")
