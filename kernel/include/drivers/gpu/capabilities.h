/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_GPU_CAPABILITIES_H
#define A20_GPU_CAPABILITIES_H
#include "core/types.h"

/* Native driver implementation roles, independent of Linux/Mesa/virgl UAPI.
 * Advertise usable binding capabilities, not hardware marketing features.
 * Discovery never grants access to memory or queues; operations retain their
 * own validation, references and stop contracts. This is not a user ABI. */
#define GPU_CAP_SCANOUT       (1ULL << 0) /* usable display output/get_info */
#define GPU_CAP_RENDER        (1ULL << 1) /* operational device rendering backend */
#define GPU_CAP_RAM_SCANOUT   (1ULL << 2) /* owning get_scanout RAM snapshots */
#define GPU_CAP_BOOT_SCANOUT  (1ULL << 3) /* retained firmware output, fallback on ties */
#define GPU_CAP_KNOWN (GPU_CAP_SCANOUT | GPU_CAP_RENDER | GPU_CAP_RAM_SCANOUT | GPU_CAP_BOOT_SCANOUT)
struct gpu_capabilities { uint64_t flags; };
#endif
