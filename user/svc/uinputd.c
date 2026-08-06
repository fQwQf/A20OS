/*
 * uinputd — user placement of the dual-placement virtio-input driver
 * (docs/hybrid-kernel/04-dual-placement.md).
 *
 * Same shared protocol source as the kernel probe
 * (kernel/drivers/input/virtio_input_kprobe.c).  This shell maps the
 * whitelisted user-owned slot and reports the device identity; the
 * smoke compares its output with the kernel placement's boot log to
 * verify dual-placement semantic consistency.  Virtqueue event
 * delivery is not wired yet (single-owner init; see the design doc).
 */
#define DRV_ENV_USER 1
#include "drivers/dual/virtio_input.h"
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

#define VINPUT_DUAL_PHYS 0x10006000ULL
#define VINPUT_DUAL_SIZE 0x1000ULL

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    a20_handle_t out = si ? si->stdout_handle : A20_HANDLE_NULL;
#define UIN_LOG(msg) a20_hdl_write_buf(out, msg, sizeof(msg) - 1, (void *)0)
    if (out == A20_HANDLE_NULL)
        return 1;

    uint64_t base = drv_mmio_map(VINPUT_DUAL_PHYS, VINPUT_DUAL_SIZE, 3);
    if (!base) {
        UIN_LOG("UINPUTD: map failed\n");
        return 2;
    }

    vmmio_probe_t p;
    if (vmmio_probe(base, &p) != 0 || p.device_id != VIRTIO_INPUT_DEVICE_ID) {
        UIN_LOG("UINPUTD: no input device\n");
        return 3;
    }

    char name[64];
    uint32_t n = vinput_cfg_string(base, VIRTIO_INPUT_CFG_ID_NAME,
                                   name, sizeof(name));
    char line[96];
    int len = 0;
    const char *pfx = "UINPUTD: name=";
    for (const char *c = pfx; *c && len < (int)sizeof(line) - 2; c++)
        line[len++] = *c;
    for (uint32_t i = 0; i < n && len < (int)sizeof(line) - 2; i++)
        line[len++] = name[i];
    line[len++] = '\n';
    a20_hdl_write_buf(out, line, (uint64_t)len, (void *)0);

    if (n == 0) {
        UIN_LOG("UINPUTD: FAIL (empty name)\n");
        return 4;
    }
    UIN_LOG("UINPUTD: PASS\n");
    return 0;
}
