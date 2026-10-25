#include "liointc.h"
#include "core/errno.h"

#define ENABLE 0x28U
#define DISABLE 0x2cU
#define POLARITY 0x30U
#define EDGE 0x34U

int ls2k0300_lio_init(ls2k0300_lio_t *chip, const ls2k0300_lio_io_t *io) {
    if (!chip || !io || !io->read32 || !io->write32 || !io->write8)
        return -EINVAL;
    chip->io = *io;
    for (unsigned bank = 0; bank < 2; bank++) {
        unsigned base = bank * 0x40;
        chip->enabled[bank] = chip->edge[bank] = chip->polarity[bank] = 0;
        io->write32(io->context, base + DISABLE, ~0U);
        io->write32(io->context, base + EDGE, 0);
        io->write32(io->context, base + POLARITY, 0);
        /* Bank 0 -> INT0/HWI0, bank 1 -> INT2/HWI2, matching the DTS.
         * Unlike multicore LIOINTC variants, low four bits are reserved. */
        for (unsigned line = 0; line < 32; line++)
            io->write8(io->context, base + line, bank ? 0x40 : 0x10);
    }
    return 0;
}
int ls2k0300_lio_configure(ls2k0300_lio_t *chip, unsigned irq, unsigned type) {
    if (!chip || irq >= 64 ||
        (type != 1 && type != 2 && type != 4 && type != 8))
        return -EINVAL;
    unsigned bank = irq / 32, base = bank * 0x40;
    uint32_t bit = 1U << (irq % 32);
    if (chip->enabled[bank] & bit)
        return -EBUSY;
    uint32_t polarity = chip->polarity[bank];
    if (type == 2 || type == 8)
        polarity |= bit;
    else
        polarity &= ~bit;
    chip->polarity[bank] = polarity;
    if (type == 1 || type == 2)
        chip->edge[bank] |= bit;
    else
        chip->edge[bank] &= ~bit;
    chip->io.write32(chip->io.context, base + POLARITY, polarity);
    chip->io.write32(chip->io.context, base + EDGE, chip->edge[bank]);
    return 0;
}
int ls2k0300_lio_enable(ls2k0300_lio_t *chip, unsigned irq) {
    if (!chip || irq >= 64)
        return -EINVAL;
    uint32_t bit = 1U << (irq % 32);
    chip->enabled[irq / 32] |= bit;
    chip->io.write32(chip->io.context, irq / 32 * 0x40 + ENABLE, bit);
    return 0;
}
void ls2k0300_lio_disable(ls2k0300_lio_t *chip, unsigned irq) {
    if (!chip || irq >= 64)
        return;
    uint32_t bit = 1U << (irq % 32);
    chip->enabled[irq / 32] &= ~bit;
    chip->io.write32(chip->io.context, irq / 32 * 0x40 + DISABLE, bit);
}
void ls2k0300_lio_dispatch(ls2k0300_lio_t *chip, unsigned bank,
                           uint32_t pending, void (*dispatch)(uint32_t)) {
    if (!chip || bank >= 2 || !dispatch)
        return;
    pending &= chip->enabled[bank];
    while (pending) {
        unsigned line = __builtin_ctz(pending);
        uint32_t bit = 1U << line;
        pending &= ~bit;
        if (!(chip->enabled[bank] & bit))
            continue;
        /* INTCLR also masks the line. Rearm before calling the handler so a
         * subsequent edge can latch during its execution. The handler may
         * disable the line; no trailing unmask may override that decision.
         * Level sources are cleared by their device handlers. */
        int edge = !!(chip->edge[bank] & bit);
        if (edge) {
            chip->io.write32(chip->io.context, bank * 0x40 + DISABLE, bit);
            chip->io.write32(chip->io.context, bank * 0x40 + ENABLE, bit);
        }
        dispatch(bank * 32 + line);
    }
}
