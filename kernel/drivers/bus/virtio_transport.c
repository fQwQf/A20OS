#include "drivers/bus/virtio_transport.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/block/virtio_blk.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/timer.h"
#include "proc/proc.h"

int virtio_transport_reset(virtio_transport_t *transport, uint32_t timeout_ms)
{
    if (!transport || !transport->read32 || !transport->write32 ||
        !timeout_ms || timeout_ms > 1000) return -EINVAL;
    uint64_t frequency = clock_ticks_per_sec();
    /* ceil(frequency * timeout_ms / 1000), without overflowing at high Hz. */
    uint64_t interval = (frequency / 1000) * timeout_ms +
        ((frequency % 1000) * timeout_ms + 999) / 1000;
    uint64_t start = clock_get_ticks();
    transport->write32(transport, VIRTIO_MMIO_STATUS, 0);
    mb();
    for (uint32_t polls = 0; polls < 1000000U; polls++) {
        if (transport->read32(transport, VIRTIO_MMIO_STATUS) == 0) {
            mb();
            return 0;
        }
        if (interval && clock_get_ticks() - start >= interval) break;
        arch_cpu_relax();
        if ((polls & 255U) == 255U && proc_current()) proc_yield();
    }
    return -ETIMEDOUT;
}
