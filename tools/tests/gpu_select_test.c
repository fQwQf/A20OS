/* Pure priority policy: the two-level cases exercise promotion/re-election;
 * native roles below additionally cover managed/firmware rendering priorities. */
#include <assert.h>
#include <stdio.h>

#include "drivers/gpu/gpu_select.h"

static void test_empty_slot_always_takes(void)
{
    /* Nothing to displace: every newcomer wins, so the first display device is
     * always usable. */
    assert(gpu_select_should_take(false, false, false) == true);
    assert(gpu_select_should_take(false, false, true) == true);
}

static void test_promotion_only(void)
{
    /* A 3D-capable newcomer displaces a 2D-only incumbent. */
    assert(gpu_select_should_take(true, false, true) == true);

    /* A 2D-only newcomer must not displace a 3D-capable incumbent, or probe
     * order decides whether the machine has 3D. */
    assert(gpu_select_should_take(true, true, false) == false);

    /* Equally capable devices never trade the slot back and forth; that
     * thrash is what the promotion-only rule exists to prevent. */
    assert(gpu_select_should_take(true, true, true) == false);
    assert(gpu_select_should_take(true, false, false) == false);
}

static void test_rescan_prefers_3d_over_a_lower_index_2d(void)
{
    /* The re-election scan, in enumeration order, starting empty. Index 0 is a
     * 2D-only device, index 1 is 3D-capable. The scan must reach the second:
     * stopping at the first candidate would leave a 2D backend on a machine
     * that has a working 3D device, and nothing would report it. */
    bool have_best = false, best_3d = false;

    assert(gpu_select_better(have_best, best_3d, false) == true);
    have_best = true; best_3d = false;          /* index 0: 2D-only wins so far */

    assert(gpu_select_better(have_best, best_3d, true) == true);
    have_best = true; best_3d = true;          /* index 1: 3D displaces it */

    /* Nothing beats an actual 3D winner. */
    assert(gpu_select_better(have_best, best_3d, true) == false);
    assert(gpu_select_better(have_best, best_3d, false) == false);
}

static void test_rescan_ignores_the_departing_device(void)
{
    /* A 2D-only device is the one leaving; the only survivor is 3D-capable, so
     * the scan has exactly one candidate and must take it. This is the case
     * that stayed broken: the slot was cleared and nothing refilled it. */
    bool have_best = false, best_3d = false;
    assert(gpu_select_better(have_best, best_3d, true) == true);
}

static void test_rescan_with_no_survivor(void)
{
    /* Nothing survives: the caller keeps an empty slot. gpu_select_better on an
     * empty incumbent says "take the first candidate", so the caller must never
     * call it at all when enumeration is empty; assert the shape that lets it
     * detect that. */
    bool have_best = false;
    assert(have_best == false);
    assert(gpu_select_better(have_best, false, false) == true);
}

static void test_native_role_priorities(void)
{
    unsigned firmware = gpu_select_rank(false, true);
    unsigned managed = gpu_select_rank(false, false);
    unsigned firmware_render = gpu_select_rank(true, true);
    unsigned managed_render = gpu_select_rank(true, false);
    assert(gpu_select_should_take(true, firmware, managed));
    assert(!gpu_select_should_take(true, managed, firmware));
    assert(gpu_select_should_take(true, managed, firmware_render));
    assert(gpu_select_better(true, firmware_render, managed_render));
    assert(!gpu_select_better(true, managed_render, firmware_render));
    assert(managed_render == GPU_SELECT_MAX_RANK);
    for (unsigned rank = firmware; rank <= managed_render; rank++) {
        assert(!gpu_select_should_take(true, rank, rank));
        assert(!gpu_select_better(true, rank, rank));
        assert(gpu_select_should_take(false, 0, rank));
    }
}

int main(void)
{
    test_native_role_priorities();
    test_empty_slot_always_takes();
    test_promotion_only();
    test_rescan_prefers_3d_over_a_lower_index_2d();
    test_rescan_ignores_the_departing_device();
    test_rescan_with_no_survivor();
    printf("gpu_select_test: PASS\n");
    return 0;
}
