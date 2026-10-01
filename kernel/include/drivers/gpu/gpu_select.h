/*
 * Which display device owns the DRM backend.
 *
 * Split out of gpu_core.c, and free of kernel types, for the same reason
 * drm_geom.h is: the rule below decides whether a machine ends up with a 3D
 * backend or silently loses DRM altogether, and neither failure is visible from
 * the caller -- every ioctl just answers -ENODEV. It is pure logic over two
 * booleans, so tools/tests/gpu_select_test.c can drive it directly instead of
 * the rule being asserted by inspection.
 *
 * Both gpu_device_register() and gpu_device_unregister() must consult this, or
 * a device arriving and a device leaving can disagree about the same set and the
 * binding depends on which happened to run last.
 */
#ifndef _DRIVERS_GPU_GPU_SELECT_H
#define _DRIVERS_GPU_GPU_SELECT_H

#include <stdbool.h>

/* Decide the default GPU from the incumbent and the newcomer.
 *
 * `has_incumbent` is false when the slot is empty. Promotion only: a 3D-capable
 * newcomer displaces a 2D-only incumbent, but two equally capable devices never
 * trade the slot, so probe order cannot churn the DRM binding.
 *
 * Returns true when the newcomer should take the slot. With no incumbent it
 * always does, which is what makes a first display device win unconditionally.
 */
static inline bool gpu_select_should_take(bool has_incumbent, bool incumbent_3d,
                                          bool newcomer_3d)
{
    if (!has_incumbent)
        return true;
    return newcomer_3d && !incumbent_3d;
}

/* Fold a candidate into the running best, for the re-election scan.
 *
 * Returns true when the candidate replaced the incumbent best. A 3D-capable
 * candidate always wins; among equals the incumbent best is kept, so the scan
 * result does not depend on enumeration order beyond preferring 3D.
 */
static inline bool gpu_select_better(bool have_best, bool best_3d,
                                     bool cand_3d)
{
    if (!have_best)
        return true;
    return cand_3d && !best_3d;
}

#endif /* _DRIVERS_GPU_GPU_SELECT_H */
