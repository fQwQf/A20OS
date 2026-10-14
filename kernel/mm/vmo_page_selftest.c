#include "cg/cgroup.h"
#include "cg/cgroup_impl.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "mm/frame.h"
#include "mm/vmo.h"

#ifdef CONFIG_VMO_PAGE_TEST

static cg_node_t vmo_test_fail_cgroup;
static cg_node_t vmo_test_ok_cgroup;

static void vmo_test_require(int condition, const char *what)
{
    if (!condition)
        panic("VMO_PAGE_TEST: %s", what);
}

void vmo_page_selftest_run(void)
{
    cg_mem_init(&vmo_test_fail_cgroup.res.mem);
    vmo_test_fail_cgroup.res.mem.limit = 0;
    cg_mem_init(&vmo_test_ok_cgroup.res.mem);
    vmo_test_ok_cgroup.res.mem.limit = 8;

    struct vmo *vmo = vmo_create(VMO_ANONYMOUS, 2 * PAGE_SIZE, 0);
    vmo_test_require(vmo != NULL, "create failure-injection VMO");
    pfn_t page = PFN_NONE;
    int rc = vmo_get_page_charged(vmo, 0, &vmo_test_fail_cgroup, &page);
    vmo_test_require(rc == -12, "zero-limit cgroup charge must fail");
    vmo_test_require(page == PFN_NONE, "failed charge must not publish output");
    vmo_test_require(vmo->pages[0] == PFN_NONE && vmo->phys_size == 0 &&
                     vmo->charged_pages == 0,
                     "failed charge must not allocate or publish a page");
    vmo_test_require(vmo_test_fail_cgroup.res.mem.rss == 0 &&
                     vmo_test_fail_cgroup.res.mem.failcnt == 1,
                     "real cgroup charge failure accounting");
    vmo_release(vmo);

    vmo = vmo_create(VMO_ANONYMOUS, 2 * PAGE_SIZE, 0);
    vmo_test_require(vmo != NULL, "create resize VMO");
    rc = vmo_get_page_charged(vmo, 0, &vmo_test_ok_cgroup, &page);
    vmo_test_require(rc == 0 && page != PFN_NONE,
                     "charged first touch allocates canonical page");
    vmo_test_require(vmo_test_ok_cgroup.res.mem.rss == 1 &&
                     vmo->phys_size == PAGE_SIZE && vmo->charged_pages == 1,
                     "charged first touch accounting");
    vmo_test_require(vmo_resize(vmo, 0) == 0,
                     "shrink VMO to zero pages");
    vmo_test_require(vmo->page_count == 0 && vmo->size == 0 &&
                     vmo->phys_size == 0 && vmo->charged_pages == 0 &&
                     vmo_test_ok_cgroup.res.mem.rss == 0,
                     "shrink releases frame and cgroup charge");
    vmo_test_require(vmo_resize(vmo, 2 * PAGE_SIZE) == 0,
                     "grow VMO after shrink");
    vmo_test_require(vmo->page_count == 2 && vmo->size == 2 * PAGE_SIZE &&
                     vmo_peek_page(vmo, 0) == PFN_NONE,
                     "grow publishes empty canonical slots");
    vmo_release(vmo);

    vmo = vmo_create(VMO_ANONYMOUS, PAGE_SIZE, 0);
    vmo_test_require(vmo != NULL, "create destroy VMO");
    rc = vmo_get_page_charged(vmo, 0, &vmo_test_ok_cgroup, &page);
    vmo_test_require(rc == 0 && vmo_test_ok_cgroup.res.mem.rss == 1,
                     "charged page before final release");
    vmo_release(vmo);
    vmo_test_require(vmo_test_ok_cgroup.res.mem.rss == 0,
                     "final destroy uncharges remaining pages");
    printf("VMO_PAGE_TEST: PASS charge_fail=1 shrink=1 grow=1 destroy_uncharge=1\n");
}

#endif
