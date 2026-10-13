#ifdef CONFIG_SHARED_DEVICE_CALL
#include "core/shared_mmio.h"
#include "core/shared_device_call.h"
#include "core/errno.h"
#include "core/panic.h"

struct mmio_request { uintptr_t address; uint64_t value; unsigned width; };
static int mmio_operation(uint32_t write, uint64_t argument, uint64_t *result) {
    const struct mmio_request *request = (const void *)(uintptr_t)argument;
    uintptr_t address = request->address;
    /* Raw volatile accesses: using the driver HAL here would recurse into
     * the mailbox. No locks, waits, allocations or console in this leaf. */
#define ACCESS(type) do { \
    if (write) *(volatile type *)address = (type)request->value; \
    else *result = *(const volatile type *)address; \
} while (0)
    switch (request->width) {
    case 1: ACCESS(uint8_t); break;
    case 2: ACCESS(uint16_t); break;
    case 4: ACCESS(uint32_t); break;
    case 8: ACCESS(uint64_t); break;
    default: return -EINVAL;
    }
#undef ACCESS
    return 0;
}
static uint64_t mmio_call(unsigned owner, uintptr_t address, unsigned width,
                          uint64_t value, unsigned write) {
    if ((width != 1 && width != 2 && width != 4 && width != 8) ||
        (address & (width - 1)))
        panic("shared MMIO: invalid width/alignment");
    /* Stack and request remain live until DONE; mailbox pins the caller. */
    struct mmio_request request = { address, value, width };
    uint64_t result = 0;
    int status = shared_device_call(owner, mmio_operation, write,
                                    (uintptr_t)&request, &result);
    if (status) panic("shared MMIO: owner=%u status=%d", owner, status);
    return result;
}
uint64_t shared_mmio_read(unsigned owner, const volatile void *address, unsigned width) {
    return mmio_call(owner, (uintptr_t)address, width, 0, 0);
}
void shared_mmio_write(unsigned owner, volatile void *address, unsigned width, uint64_t value) {
    (void)mmio_call(owner, (uintptr_t)address, width, value, 1);
}
#endif
