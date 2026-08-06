/*
 * uinputd — user placement of the dual-placement virtio-input driver
 * (docs/hybrid-kernel/04-dual-placement.md).
 *
 * Same shared protocol source as the kernel probe
 * (kernel/drivers/input/virtio_input_kprobe.c).  This shell owns the
 * device at runtime: full init (status/negotiation), event virtqueue
 * over a drv_dma buffer, IRQ -> EventQ delivery.  It prints each
 * EV_KEY press and exits PASS on the first one (smoke-dual-input
 * injects a key through the QEMU monitor).
 */
#define DRV_ENV_USER 1
#include "drivers/dual/virtio_input.h"
#include "drivers/dual/virtq.h"
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

#define VINPUT_DUAL_PHYS 0x10006000ULL
#define VINPUT_DUAL_SIZE 0x1000ULL
#define VINPUT_DUAL_IRQ  6u  /* virtio-mmio slot 5: irq_base(1) + 5 */

#define IRQ_TAG 0x55494E49ULL /* "UINI" */

static a20_handle_t g_out = A20_HANDLE_NULL;

static void log_str(const char *s)
{
    if (g_out != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_out, s, a20_strlen(s), (void *)0);
}

static void log_dec(uint32_t v)
{
    char b[12];
    int n = 0;
    if (v == 0) {
        b[n++] = '0';
    } else {
        char t[12];
        int m = 0;
        while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
        while (m) b[n++] = t[--m];
    }
    if (g_out != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_out, b, (uint64_t)n, (void *)0);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 1;

    uint64_t base = drv_mmio_map(VINPUT_DUAL_PHYS, VINPUT_DUAL_SIZE, 3);
    if (!base) {
        log_str("UINPUTD: map failed\n");
        return 2;
    }

    vmmio_probe_t p;
    if (vmmio_probe(base, &p) != 0 || p.device_id != VIRTIO_INPUT_DEVICE_ID) {
        log_str("UINPUTD: no input device\n");
        return 3;
    }

    char name[64];
    uint32_t n = vinput_cfg_string(base, VIRTIO_INPUT_CFG_ID_NAME,
                                   name, sizeof(name));
    log_str("UINPUTD: name=");
    log_str(name);
    log_str("\n");
    if (n == 0)
        return 4;

    /* Own the device: full init is destructive and single-owner. */
    if (vinput_dev_init(base) != 0) {
        log_str("UINPUTD: dev init failed\n");
        return 5;
    }

    drv_dma_t dma;
    if (drv_dma_alloc(&dma, 1) != 0) {
        log_str("UINPUTD: dma alloc failed\n");
        return 6;
    }

    virtq_t evq;
    if (virtq_init(&evq, base, 0 /* eventq */, 8, &dma) != 0) {
        drv_mmio_write32(base, VMMIO_QUEUE_SEL, 0);
        log_str("UINPUTD: virtq init failed, num_max=");
        log_dec(drv_mmio_read32(base, VMMIO_QUEUE_NUM_MAX));
        log_str(" pa=");
        log_dec((uint32_t)drv_dma_phys(&dma, 0));
        log_str("\n");
        return 7;
    }

    /* Post all 8 event buffers in the page's data area (queues are
     * live from QUEUE_READY; DRIVER_OK comes after queue setup). */
    uint64_t page_pa = drv_dma_phys(&dma, 0);
    for (uint32_t i = 0; i < 8; i++)
        virtq_post_inbuf(&evq, i, page_pa + evq.buf_off + i * 8, 8);
    vinput_driver_ok(base);
    virtq_notify(&evq);

    a20_handle_t eq;
    if (a20_event_queue_create(&eq) != A20_OK)
        return 8;
    if (a20_device_irq_listen(VINPUT_DUAL_IRQ, eq, IRQ_TAG) != A20_OK) {
        log_str("UINPUTD: irq listen failed\n");
        return 9;
    }
    log_str("UINPUTD: ready\n");

    for (;;) {
        a20_event_t ev;
        a20_time_t t3 = { .secs = 3, .nsecs = 0 };
        a20_status_t wr = a20_event_wait(eq, t3, &ev);
        if (wr == -A20_ERR_TIMED_OUT) {
            volatile virtq_avail_t *av = virtq_avail(&evq);
            volatile virtq_used_t *us = virtq_used(&evq);
            log_str("UINPUTD: diag avail=");
            log_dec(av->idx);
            log_str(" used=");
            log_dec(us->idx);
            log_str(" last=");
            log_dec(evq.last_used);
            log_str(" intr=");
            log_dec(drv_mmio_read32(base, VMMIO_INTR_STATUS));
            log_str(" pa=");
            log_dec((uint32_t)(page_pa >> 20));
            log_str("M rdy=");
            drv_mmio_write32(base, VMMIO_QUEUE_SEL, 0);
            log_dec(drv_mmio_read32(base, VMMIO_QUEUE_READY));
            log_str("\n");
            continue;
        }
        if (wr < 0)
            continue;
        if (ev.user_data != IRQ_TAG)
            continue;

        virtq_irq_ack(&evq);
        a20_device_irq_ack(VINPUT_DUAL_IRQ);

        uint32_t id, len;
        while (virtq_poll_used(&evq, &id, &len)) {
            if (len == sizeof(virtio_input_event_t)) {
                volatile virtio_input_event_t *e =
                    (volatile virtio_input_event_t *)(uintptr_t)
                    (drv_dma_va(&dma, 0) + evq.buf_off + id * 8);
                log_str("UINPUTD: ev type=");
                log_dec(e->type);
                log_str(" code=");
                log_dec(e->code);
                log_str(" value=");
                log_dec(e->value);
                log_str("\n");
                if (e->type == EV_KEY && e->value == 1) {
                    log_str("UINPUTD: PASS\n");
                    return 0;
                }
            }
            /* repost the consumed buffer */
            virtq_post_inbuf(&evq, id, page_pa + evq.buf_off + id * 8, 8);
            virtq_notify(&evq);
        }
    }
}
