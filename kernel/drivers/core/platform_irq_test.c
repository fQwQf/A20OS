/*
 * A20OS platform-bus IRQ resource channel self-check.
 *
 * WHAT THIS DOES AND DOES NOT PROVE
 * ----------------------------------
 * The claim the DW-SDIO work rests on is two-sided:
 *
 *   1. a platform device that published a RES_IRQ can be claimed and served --
 *      board resource -> platform device -> request_irq() -> handler;
 *   2. a platform device that published no RES_IRQ falls back to polling
 *      cleanly, with no error, no partial claim and no changed behaviour.
 *
 * QEMU has no dw-mshc model and no board in this tree routes an SDIO line, so
 * the controller-side half (does an mshc actually raise the interrupt on DTO?)
 * cannot be exercised here and is NOT exercised here.  What this test does
 * exercise is the half that is pure kernel code and is therefore wrong-prone
 * in its own right: the resource channel, the claim, the dispatch hand-off,
 * the release, the double-claim refusal, and the driver's mode decision for
 * both the "line published" and "no line published" cases.
 *
 * The dispatch is simulated with driver_irq_dispatch(), which is exactly the
 * entry an architecture calls after it has read an interrupt id.  Nothing here
 * claims the device raises a line; it claims that IF a board raises one that
 * platform_device_irq() named, the handler runs.
 *
 * The mode decision is asserted through dw_sdio_pick_irq_mode() and
 * dw_sdio_poll_forced(), which live in drivers/block/dw_sdio.h precisely so
 * this test reaches the driver's real rule rather than a copy of it -- the
 * generic profile ships dw_sdio as a .a20drv package and does not link the
 * driver into the kernel this test runs in.
 */
#include "drivers/core/platform_irq_test.h"

#ifdef CONFIG_PLATFORM_IRQ_TEST

#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/block/dw_sdio.h"
#include "core/bootargs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"

/* A vendor/device pair no driver in the tree claims, so registering these
 * devices cannot disturb a real board's binding.  0xA20A is the lifecycle
 * test's; these are the next two. */
#define IRQTEST_VENDOR_WITH_IRQ    0xA20BU
#define IRQTEST_VENDOR_WITHOUT_IRQ 0xA20CU
#define IRQTEST_DEVICE             0x0001U

/* Line search.  The IRQ registry is a fixed 256-entry table and a board may
 * already own part of it, so rather than assume a line this test claims the
 * first one request_irq() accepts -- which is itself the property being
 * tested.  The high end is tried first because a board's own devices, not its
 * PLIC plumbing, are what occupy the low lines. */
#define IRQTEST_LINE_FIRST 255U
#define IRQTEST_LINE_LAST  200U

static volatile int g_irqtest_handler_hits;
static void *g_irqtest_handler_priv;

static int irqtest_handler(int irq, void *priv)
{
    (void)irq;
    g_irqtest_handler_hits++;
    g_irqtest_handler_priv = priv;
    return 0;
}

static int irqtest_claim_free_line(uint32_t *out_line)
{
    for (uint32_t irq = IRQTEST_LINE_FIRST; irq > IRQTEST_LINE_LAST; irq--) {
        /* out_line is request_irq()'s priv token, not a write-back slot, so
         * the line the registry accepted has to be recorded here -- otherwise
         * the caller publishes and dispatches a line nobody claimed. */
        if (request_irq(irq, irqtest_handler, 0, out_line) == 0) {
            *out_line = irq;
            return 0;
        }
    }
    return -EBUSY;
}

int platform_irq_test_run(void)
{
    int pass = 1;
    int with_irq = 0, without_irq = 0;

    static resource_t res_with_irq[2];
    static resource_t res_without_irq[1];
    static platform_device_t dev_with_irq;
    static platform_device_t dev_without_irq;
    static platform_device_t dev_range_irq;
    static resource_t res_range_irq[1];

    kinfo("[PLATFORM-IRQ] starting platform IRQ resource self-check\n");

    /* ---- 1. A device whose board published a line ------------------- */
    res_with_irq[0].type  = RES_MMIO;
    res_with_irq[0].start = 0x10000000UL;
    res_with_irq[0].end   = 0x10000FFFUL;
    res_with_irq[0].flags = IORESOURCE_MMIO_32BIT;
    res_with_irq[1].type  = RES_IRQ;
    res_with_irq[1].start = IRQTEST_LINE_FIRST;       /* patched to the real
                                                       * line once claimed */
    res_with_irq[1].end   = res_with_irq[1].start;

    dev_with_irq.dev.name      = "irqtest-with-irq";
    dev_with_irq.dev.res       = res_with_irq;
    dev_with_irq.dev.res_count = 2;
    dev_with_irq.dev.state     = DEV_STATE_UNINIT;
    dev_with_irq.id.vendor     = IRQTEST_VENDOR_WITH_IRQ;
    dev_with_irq.id.device     = IRQTEST_DEVICE;

    res_without_irq[0].type  = RES_MMIO;
    res_without_irq[0].start = 0x10010000UL;
    res_without_irq[0].end   = 0x10010FFFUL;
    res_without_irq[0].flags = IORESOURCE_MMIO_32BIT;

    dev_without_irq.dev.name      = "irqtest-without-irq";
    dev_without_irq.dev.res       = res_without_irq;
    dev_without_irq.dev.res_count = 1;
    dev_without_irq.dev.state     = DEV_STATE_UNINIT;
    dev_without_irq.id.vendor     = IRQTEST_VENDOR_WITHOUT_IRQ;
    dev_without_irq.id.device     = IRQTEST_DEVICE;

    /* A RES_IRQ whose end != start is a range, not a line. */
    res_range_irq[0].type  = RES_IRQ;
    res_range_irq[0].start = 32;
    res_range_irq[0].end   = 63;

    dev_range_irq.dev.name      = "irqtest-range-irq";
    dev_range_irq.dev.res       = res_range_irq;
    dev_range_irq.dev.res_count = 1;
    dev_range_irq.dev.state     = DEV_STATE_UNINIT;
    dev_range_irq.id.vendor     = IRQTEST_VENDOR_WITH_IRQ;
    dev_range_irq.id.device     = 0x0002U;

    if (platform_device_register(&dev_with_irq) != 0) {
        kerr("[PLATFORM-IRQ] device_register(with IRQ) failed\n");
        pass = 0;
        goto out;
    }
    if (platform_device_register(&dev_without_irq) != 0) {
        kerr("[PLATFORM-IRQ] device_register(no IRQ) failed\n");
        pass = 0;
        goto out;
    }

    /* ---- 2. Absent resource is -ENODEV, not an error ---------------- */
    without_irq = platform_device_irq(&dev_without_irq.dev);
    if (without_irq != -ENODEV) {
        kerr("[PLATFORM-IRQ] a device with no RES_IRQ reported %d, "
             "expected -ENODEV\n", without_irq);
        pass = 0;
        goto out;
    }
    /* ...and that is the whole fallback: the driver's rule turns it into
     * polling, with no claim attempted and nothing to undo. */
    if (dw_sdio_pick_irq_mode(without_irq, 0) != 0) {
        kerr("[PLATFORM-IRQ] a device with no RES_IRQ did not fall back "
             "to polling\n");
        pass = 0;
        goto out;
    }

    /* ---- 3. A published line is claimable and served ---------------- */
    {
        uint32_t line = 0;
        if (irqtest_claim_free_line(&line) != 0) {
            kerr("[PLATFORM-IRQ] no free IRQ line to claim\n");
            pass = 0;
            goto out;
        }
        /* Publish the line the claim actually got, so the resource channel
         * under test hands back the same number the registry holds. */
        res_with_irq[1].start = line;
        res_with_irq[1].end   = line;

        with_irq = platform_device_irq(&dev_with_irq.dev);
        if (with_irq < 0 || (uint32_t)with_irq != line) {
            kerr("[PLATFORM-IRQ] RES_IRQ %u was reported as %d\n",
                 line, with_irq);
            pass = 0;
            goto out;
        }
        /* With a real line and no override, the driver takes the interrupt. */
        if (dw_sdio_pick_irq_mode(with_irq, 0) != 1) {
            kerr("[PLATFORM-IRQ] a published line did not select the "
                 "interrupt path\n");
            pass = 0;
            goto out;
        }
        /* a20.dw-sdio.poll=1 overrides the published line. */
        if (dw_sdio_pick_irq_mode(with_irq, 1) != 0) {
            kerr("[PLATFORM-IRQ] a20.dw-sdio.poll=1 did not force polling\n");
            pass = 0;
            goto out;
        }

        /* ---- 4. Dispatch reaches the handler ------------------------ */
        g_irqtest_handler_hits = 0;
        g_irqtest_handler_priv = NULL;
        driver_irq_dispatch(line);
        if (g_irqtest_handler_hits != 1 ||
            g_irqtest_handler_priv != &line) {
            kerr("[PLATFORM-IRQ] dispatch did not reach the handler "
                 "(hits=%d)\n", g_irqtest_handler_hits);
            pass = 0;
            goto out;
        }

        /* ---- 5. A second claim of a live line is refused ------------ */
        {
            void *token = NULL;
            if (request_irq(line, irqtest_handler, 0, &token) != -EBUSY) {
                kerr("[PLATFORM-IRQ] a second claim of a live line was "
                     "accepted\n");
                pass = 0;
                goto out;
            }
        }

        /* ---- 6. free_irq really detaches the handler ---------------- */
        free_irq(line, &line);
        driver_irq_dispatch(line);
        if (g_irqtest_handler_hits != 1) {
            kerr("[PLATFORM-IRQ] dispatch still reached a released handler\n");
            pass = 0;
            goto out;
        }
        /* A released line is claimable again: the test leaves no residue. */
        if (request_irq(line, irqtest_handler, 0, &line) != 0) {
            kerr("[PLATFORM-IRQ] a released line stayed claimed\n");
            pass = 0;
            goto out;
        }
        free_irq(line, &line);
    }

    /* ---- 7. A malformed RES_IRQ is rejected, not silently truncated -- */
    if (platform_device_register(&dev_range_irq) != 0) {
        kerr("[PLATFORM-IRQ] device_register(range IRQ) failed\n");
        pass = 0;
        goto out;
    }
    if (platform_device_irq(&dev_range_irq.dev) != -EINVAL) {
        kerr("[PLATFORM-IRQ] an IRQ range was accepted as a line\n");
        pass = 0;
        goto out;
    }

    /* ---- 8. The cmdline parser the driver runs ------------------------ */
    if (!dw_sdio_poll_forced("a20.dw-sdio.poll=1")) {
        kerr("[PLATFORM-IRQ] a20.dw-sdio.poll=1 was not recognised\n");
        pass = 0;
        goto out;
    }
    if (!dw_sdio_poll_forced("quiet a20.dw-sdio.poll=1 ro root=LABEL=x")) {
        kerr("[PLATFORM-IRQ] a20.dw-sdio.poll=1 was missed mid-cmdline\n");
        pass = 0;
        goto out;
    }
    if (dw_sdio_poll_forced("a20.dw-sdio.poll=0")) {
        kerr("[PLATFORM-IRQ] a20.dw-sdio.poll=0 requested polling\n");
        pass = 0;
        goto out;
    }
    if (dw_sdio_poll_forced("a20.dw-sdio.polling=1")) {
        kerr("[PLATFORM-IRQ] a20.dw-sdio.polling=1 was mistaken for the key\n");
        pass = 0;
        goto out;
    }
    if (dw_sdio_poll_forced(NULL) || dw_sdio_poll_forced("")) {
        kerr("[PLATFORM-IRQ] an empty cmdline requested polling\n");
        pass = 0;
        goto out;
    }
    /* The real command line, so the boot this runs in proves the key does not
     * fire by accident.  Its value is not asserted -- a run that booted with
     * a20.dw-sdio.poll=1 on the cmdline is entitled to report 1. */
    kinfo("[PLATFORM-IRQ] live cmdline selects poll=%d\n",
          dw_sdio_poll_forced(bootargs_get()));

out:
    /* Objects live in statics, but they are still in the global registries
     * after an early goto above, so always take them back out. */
    platform_device_unregister(&dev_range_irq);
    platform_device_unregister(&dev_without_irq);
    platform_device_unregister(&dev_with_irq);

    if (pass) {
        kinfo("PLATFORM_IRQ_TEST: PASS\n");
        return 0;
    }
    kerr("PLATFORM_IRQ_TEST: FAIL\n");
    return -1;
}

#endif /* CONFIG_PLATFORM_IRQ_TEST */