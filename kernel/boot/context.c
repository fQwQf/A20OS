#include "boot/context.h"
#include "core/errno.h"
#include "core/string.h"

static boot_context_t context;
const boot_context_t *boot_context_get(void) {
    return &context;
}
int boot_context_publish(const boot_context_t *in) {
    if (!in || in->protocol < BOOT_PROTOCOL_STATIC ||
        in->protocol > BOOT_PROTOCOL_UEFI || (in->fdt && !in->fdt_available) ||
        (!in->fdt && in->fdt_available))
        return -EINVAL;
    if (context.phase != BOOT_PHASE_RESET)
        return -EALREADY;
    context = *in;
    context.phase = BOOT_PHASE_HANDOFF;
    return 0;
}
int boot_context_advance(enum boot_phase expected, enum boot_phase next) {
    if (expected < BOOT_PHASE_HANDOFF || next != expected + 1 ||
        next > BOOT_PHASE_READY)
        return -EINVAL;
    if (context.phase != expected)
        return -EALREADY;
    context.phase = next;
    return 0;
}
int boot_context_set_hardware_root(const fw_node_t *root) {
    if (!fw_node_valid(root) || context.phase < BOOT_PHASE_HANDOFF ||
        context.phase >= BOOT_PHASE_DEVICES)
        return -EINVAL;
    if (fw_node_valid(&context.hardware_root))
        return -EEXIST;
    context.hardware_root = *root;
    return 0;
}
