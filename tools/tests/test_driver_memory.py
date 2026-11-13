"""Physical storage guards: production core/ACPI, pthread races and boot import."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

if __package__:
    from .test_firmware_model import ROOT, STUBS
    from .test_driver_workqueue import LOCK_STUB
else:
    from test_firmware_model import ROOT, STUBS
    from test_driver_workqueue import LOCK_STUB


class DriverMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="a20-driver-memory-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        stubs = {**STUBS, "core/lock.h": LOCK_STUB,
                 "core/sync.h": '''#pragma once
#include <assert.h>
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#define MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline void mutex_init(mutex_t *m) {
 pthread_mutexattr_t a;assert(!pthread_mutexattr_init(&a));
 assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));
 assert(!pthread_mutex_init(m,&a));assert(!pthread_mutexattr_destroy(&a));
}
static inline void mutex_lock(mutex_t *m){assert(!pthread_mutex_lock(m));}
static inline void mutex_unlock(mutex_t *m){assert(!pthread_mutex_unlock(m));}
static inline int mutex_trylock(mutex_t *m){return !pthread_mutex_trylock(m);}
''', "mm/frame.h": '''#pragma once
#include "core/types.h"
#define PFA_MAX_RANGES 64
#define PAGE_SIZE 4096
typedef struct { uint64_t base,end; } pfa_range_t;
typedef struct { size_t nr_ranges; pfa_range_t ranges[PFA_MAX_RANGES]; } pfa_t;
extern pfa_t pfa;
''', "net/socket/socket_internal.h":
                 '#include "core/types.h"\nvoid netlink_uevent_emit(const char *,const char *,const char *,uint64_t);\n'}
        for name, content in stubs.items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        source = (ROOT / "tools/tests/driver_resource_host.c").read_text()
        marker = "int main(void) {"
        assert source.count(marker) == 1
        cases = (ROOT / "tools/tests/driver_memory_host.c").read_text()
        source = source.replace(marker, cases + '\nint main(int argc,char **argv) {')
        marker = "    int baseline = allocations;"
        assert source.count(marker) == 1
        source = source.replace(marker, marker + '''
    if(argc==2) {
        memory_case(argv[1]);
        assert(allocations==baseline+(!strncmp(argv[1],"boot-",5)?1:0));
        assert(a.references==1&&b.references==1);
        return 0;
    }
''')
        fixture = work / "fixture.c"
        fixture.write_text(source)
        cls.binary = work / "test"
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-DCONFIG_NR_CPUS=2", "-DCONFIG_ACPI", "-I" + str(work),
            "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"),
            str(fixture), str(ROOT / "kernel/drivers/core/driver_core.c"),
            str(ROOT / "kernel/drivers/core/driver_class.c"),
            str(ROOT / "kernel/drivers/core/memory_boot.c"),
            str(ROOT / "kernel/firmware/acpi/tables.c"),
            str(ROOT / "kernel/firmware/node.c"), "-o", str(cls.binary),
        ], check=True)

    def run_case(self, case):
        subprocess.run([str(self.binary), case], check=True, timeout=15,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    def test_copied_ranges_endpoints_and_independent_lifetimes(self):
        self.run_case("intervals")

    def test_failed_batch_has_no_partial_protection(self):
        self.run_case("failures")

    def test_managed_mapper_never_touches_protected_storage(self):
        self.run_case("mapping")

    def test_reservation_and_basic_claim_race(self):
        self.run_case("claim-race")

    def test_reservation_and_device_publication_race(self):
        self.run_case("publish-race")

    def test_normalized_intervals_match_byte_coverage(self):
        self.run_case("normalize")

    def test_boot_ram_and_acpi_storage_protection(self):
        self.run_case("boot-ram")
        self.run_case("boot-acpi")

    def test_boot_failed_import_is_atomic_and_retryable(self):
        for case in ["boot-table-fail", "boot-changed", "boot-bad-ram",
                     "boot-alloc-0", "boot-alloc-1"]:
            with self.subTest(case=case):
                self.run_case(case)


if __name__ == "__main__":
    unittest.main()
