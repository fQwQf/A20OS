/* Production-engine fixtures: simulated register behavior only, never a second
 * implementation of the driver. Run each scenario in an isolated process. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/block/sunxi_mmc.h"
#include "core/errno.h"

typedef struct hardware {
    uint32_t registers[0x204 / 4];
    uint32_t csd[4];
    uint32_t arguments[8];
    uint32_t status_bits;
    uint32_t voltage;
    unsigned read_commands, write_commands, block_length_commands;
    unsigned fifo_words, written_words;
    int sdsc, no_card, bad_echo, crc_error, fifo_stuck, clock_stuck;
    int bad_r1, bad_rca, no_ready, no_app, power_stuck, data_timeout;
    int reset_stuck, mask_stuck, disable_stuck, gone, clock_error;
    unsigned mmio_reads, mmio_writes, cancel_word;
    sunxi_mmc_host_t *cancel_host;
    unsigned resets;
    uint64_t ticks;
} hardware_t;
static uint32_t read_register(void *context, uint32_t reg) {
    hardware_t *hw = context;
    hw->mmio_reads++;
    if (hw->gone)
        return 0xffffffffU;
    if (reg == 0x3c)
        return hw->fifo_stuck ? (1U << 2) | (1U << 3) : 0;
    if (reg == 0x38 && hw->crc_error && hw->fifo_words == 64)
        return 1U << 7;
    if (reg == 0x200) {
        uint32_t value = 0xa5000000U | hw->fifo_words++;
        if (hw->cancel_word && hw->fifo_words == hw->cancel_word)
            __atomic_store_n(&hw->cancel_host->closing, 1, __ATOMIC_RELEASE);
        return value;
    }
    return hw->registers[reg / 4];
}
static void write_register(void *context, uint32_t reg, uint32_t value) {
    hardware_t *hw = context;
    assert(!hw->gone);
    hw->mmio_writes++;
    if (reg == 0) {
        if (value & 7)
            hw->resets++;
        hw->registers[0] = hw->reset_stuck ? value : value & ~7U;
        if ((value & 1) && !hw->reset_stuck)
            hw->registers[0x18 / 4] = 0;
        return;
    }
    if (reg == 0x30 && !value && hw->mask_stuck)
        return;
    if (reg == 0x04 && !value && hw->disable_stuck)
        return;
    if (reg == 0x38) {
        hw->registers[reg / 4] &= ~value;
        return;
    }
    if (reg == 0x200) {
        assert(value == (0x5a000000U | hw->written_words));
        hw->written_words++;
        return;
    }
    hw->registers[reg / 4] = value;
    if (reg != 0x18)
        return;
    if (value & (1U << 21)) {
        if (!hw->clock_stuck)
            hw->registers[reg / 4] &= ~(1U << 31);
        if (hw->clock_error)
            hw->registers[0x38 / 4] |= 1U << 12;
        return;
    }
    hw->registers[reg / 4] &= ~(1U << 31);
    unsigned cmd = value & 63;
    hw->registers[0x38 / 4] = (1U << 2) | (1U << 3);
    hw->registers[0x20 / 4] = 0;
    if (hw->no_card && cmd) {
        hw->registers[0x38 / 4] = 1U << 8;
        return;
    }
    switch (cmd) {
        case 0:
            break;
        case 8:
            if (hw->sdsc)
                hw->registers[0x38 / 4] = 1U << 8;
            else
                hw->registers[0x20 / 4] = hw->bad_echo ? 0x1ab : 0x1aa;
            break;
        case 55:
            hw->registers[0x20 / 4] = hw->no_app ? 0 : 1U << 5;
            break;
        case 41:
            hw->registers[0x20 / 4] = hw->voltage |
                                      (hw->power_stuck ? 0 : 1U << 31) |
                                      (hw->sdsc ? 0 : 1U << 30);
            break;
        case 2:
            break;
        case 3:
            hw->registers[0x20 / 4] = hw->bad_rca ? 0xe000 : 0x12340000;
            break;
        case 9:
            for (unsigned i = 0; i < 4; i++)
                hw->registers[(0x20 / 4) + 3 - i] = hw->csd[i];
            break;
        case 7:
            hw->registers[0x20 / 4] = hw->status_bits;
            break;
        case 13:
            hw->registers[0x20 / 4] = hw->bad_r1     ? 1U << 22
                                      : hw->no_ready ? 7U << 9
                                                     : (4U << 9) | (1U << 8);
            break;
        case 16:
            assert(hw->sdsc);
            assert(hw->registers[0x1c / 4] == 512);
            hw->block_length_commands++;
            break;
        case 17:
            assert(hw->read_commands < 8);
            hw->arguments[hw->read_commands++] = hw->registers[0x1c / 4];
            if (hw->data_timeout)
                hw->registers[0x38 / 4] = 1U << 9;
            break;
        case 24:
            assert(hw->write_commands < 8);
            hw->arguments[hw->write_commands++] = hw->registers[0x1c / 4];
            assert(value & (1U << 10));
            break;
        default:
            assert(!"unexpected SD command");
    }
}
static uint64_t ticks(void *context) {
    return ++((hardware_t *)context)->ticks;
}
static sunxi_mmc_platform_t platform = {400000, 0x200, 1};
static void init(hardware_t *hw) {
    memset(hw, 0, sizeof(*hw));
    hw->voltage = 0x00ff8000U;
    /* SDHC C_SIZE 0x12345 -> 74566 * 1024 512-byte sectors. */
    hw->csd[0] = 1U << 30;
    hw->csd[1] = 1;
    hw->csd[2] = 0x23450000;
}
static int start(hardware_t *hw, sunxi_mmc_host_t *host) {
    sunxi_mmc_io_t io = {hw, read_register, write_register, ticks, 1000};
    return sunxi_mmc_start(host, &io, &platform);
}
#ifndef SUNXI_HOST_NO_MAIN
int main(int argc, char **argv) {
    assert(argc == 2);
    int scenario = atoi(argv[1]);
    hardware_t hw;
    sunxi_mmc_host_t host;
    init(&hw);
    uint8_t data[1025];
    if (scenario == 0) { /* real SDHC protocol, multi-sector/unaligned read */
        assert(!start(&hw, &host));
        assert(host.ready && host.high_capacity &&
               host.sectors == 74566ULL * 1024);
        assert(!hw.block_length_commands);
        assert(hw.registers[0x5c / 4] & (1U << 31));
        assert(hw.registers[0x04 / 4] == (1U << 16));
        assert(!sunxi_mmc_transfer(&host, 11, data + 1, 2, 0));
        assert(hw.arguments[0] == 11 && hw.arguments[1] == 12 &&
               hw.fifo_words == 256);
        for (unsigned i = 0; i < 256; i++) {
            uint32_t value;
            memcpy(&value, data + 1 + i * 4, 4);
            assert(value == (0xa5000000U | i));
        }
        assert(!sunxi_mmc_flush(&host));
        assert(!sunxi_mmc_stop(&host));
        assert(!host.ready && !hw.registers[0x30 / 4] &&
               !hw.registers[0x04 / 4]);
        assert(sunxi_mmc_transfer(&host, 0, data, 1, 0) == -ENODEV);
    } else if (scenario == 1) { /* SDSC geometry and byte-addressed commands */
        hw.sdsc = 1;
        memset(hw.csd, 0, sizeof(hw.csd));
        hw.csd[1] = (9U << 16) | 255U;
        hw.csd[2] = (3U << 30) | (7U << 15); /* C_SIZE=1023, multiplier=7 */
        assert(!start(&hw, &host));
        assert(!host.high_capacity && host.sectors == 524288 &&
               hw.block_length_commands == 1);
        for (unsigned i = 0; i < 256; i++) {
            uint32_t value = 0x5a000000U | i;
            memcpy(data + 1 + i * 4, &value, 4);
        }
        assert(!sunxi_mmc_transfer(&host, 7, data + 1, 2, 1));
        assert(hw.arguments[0] == 7 * 512 && hw.arguments[1] == 8 * 512 &&
               hw.written_words == 256);
    } else if (scenario == 2) {
        hw.no_card = 1;
        assert(start(&hw, &host) == -ETIMEDOUT);
        assert(!host.ready);
        assert(!sunxi_mmc_stop(&host));
        assert(!hw.registers[0x04 / 4]);
    } else if (scenario == 3) {
        hw.bad_echo = 1;
        assert(start(&hw, &host) == -EOPNOTSUPP);
    } else if (scenario == 4) {
        assert(!start(&hw, &host));
        hw.fifo_stuck = 1;
        assert(sunxi_mmc_transfer(&host, 0, data, 1, 0) == -ETIMEDOUT);
        assert(!host.ready && !hw.fifo_words);
    } else if (scenario == 5) {
        assert(!start(&hw, &host));
        hw.crc_error = 1;
        assert(sunxi_mmc_transfer(&host, 0, data, 1, 0) == -EIO);
        assert(!host.ready && hw.fifo_words == 64);
        assert(sunxi_mmc_transfer(&host, 0, data, 1, 0) == -ENODEV);
    } else if (scenario == 6) {
        assert(!start(&hw, &host));
        assert(sunxi_mmc_transfer(&host, host.sectors, data, 1, 0) == -ERANGE);
        assert(sunxi_mmc_transfer(&host, host.sectors - 1, data, 2, 0) ==
               -ERANGE);
        assert(sunxi_mmc_transfer(&host, 0, data, (size_t)-1, 0) == -EINVAL);
        assert(sunxi_mmc_transfer(&host, 0, NULL, 1, 0) == -EINVAL);
        assert(host.ready && !hw.read_commands);
    } else if (scenario == 7) {
        hw.csd[3] = 1U << 12;
        assert(!start(&hw, &host));
        assert(sunxi_mmc_transfer(&host, 0, data, 1, 1) == -EROFS &&
               !hw.write_commands);
    } else if (scenario == 8) {
        hw.csd[0] = 2U << 30;
        assert(start(&hw, &host) == -EOPNOTSUPP);
    } else if (scenario == 9) {
        hw.clock_stuck = 1;
        assert(start(&hw, &host) == -ETIMEDOUT);
    } else if (scenario == 10) {
        hw.bad_r1 = 1;
        assert(start(&hw, &host) == -EIO);
    } else if (scenario == 11) {
        hw.bad_rca = 1;
        assert(start(&hw, &host) == -EIO);
    } else if (scenario == 12) {
        hw.no_app = 1;
        assert(start(&hw, &host) == -EOPNOTSUPP);
    } else if (scenario == 13) {
        hw.power_stuck = 1;
        assert(start(&hw, &host) == -ETIMEDOUT);
    } else if (scenario == 14) {
        assert(!start(&hw, &host));
        hw.data_timeout = 1;
        assert(sunxi_mmc_transfer(&host, 0, data, 1, 0) == -ETIMEDOUT &&
               !host.ready);
    } else if (scenario == 15) {
        hw.csd[1] = 63;
        hw.csd[2] = 0xffff0000;
        assert(!start(&hw, &host));
        assert(host.sectors == (1ULL << 32));
        assert(!sunxi_mmc_transfer(&host, host.sectors - 1, data, 1, 0));
        assert(hw.arguments[0] == 0xffffffffU);
    } else if (scenario == 16) {
        sunxi_mmc_io_t io = {&hw, read_register, write_register, ticks, 0};
        assert(sunxi_mmc_start(&host, &io, &platform) == -EINVAL);
        io.ticks_per_second = 1000;
        platform.clock_hz = 25000000;
        assert(sunxi_mmc_start(&host, &io, &platform) == -EINVAL);
        platform.clock_hz = 400000;
        platform.fifo_offset = 0;
        assert(sunxi_mmc_start(&host, &io, &platform) == -EINVAL);
    } else if (scenario == 17) {
        hw.no_ready = 1;
        assert(start(&hw, &host) == -ETIMEDOUT);
    } else if (scenario == 18) {
        hw.status_bits = (1U << 13) | (1U << 14); /* status, not errors */
        assert(!start(&hw, &host));
    } else if (scenario == 19) {
        hw.status_bits = 1U << 25; /* CARD_IS_LOCKED */
        assert(start(&hw, &host) == -EIO);
    } else if (scenario == 20) {
        hw.clock_stuck = 1;
        sunxi_mmc_io_t io = {&hw, read_register, write_register, ticks, 1999};
        assert(sunxi_mmc_start(&host, &io, &platform) == -ETIMEDOUT);
        assert(sunxi_mmc_stop(&host) == -ETIMEDOUT);
        /* Both the failed start handshake and stop handshake wait full 2s. */
        assert(hw.ticks >= 7996 && hw.ticks < 8020);
    } else if (scenario == 21) {
        hw.voltage = 1U << 15; /* card cannot operate on the fixed 3.3V rail */
        assert(start(&hw, &host) == -EOPNOTSUPP);
    } else if (scenario == 22) {
        for (unsigned problem = 0; problem < 6; problem++) {
            init(&hw);platform.a64_timing=problem&1;
            assert(!start(&hw,&host));
            hw.clock_stuck=problem==0;hw.reset_stuck=problem==1;
            hw.mask_stuck=problem==2;hw.disable_stuck=problem==3;hw.gone=problem==4;
            hw.clock_error=problem==5;
            if(hw.mask_stuck)hw.registers[0x30/4]=1;
            unsigned writes=hw.mmio_writes;
            assert(sunxi_mmc_stop(&host)==(hw.gone?-ENODEV:hw.clock_error?-EIO:-ETIMEDOUT));
            assert(host.closing&&!host.ready);
            if(hw.gone)assert(writes==hw.mmio_writes);
            unsigned reads=hw.mmio_reads;writes=hw.mmio_writes;
            assert(sunxi_mmc_transfer(&host,0,data,1,0)==-ENODEV&&sunxi_mmc_flush(&host)==-ENODEV);
            assert(reads==hw.mmio_reads&&writes==hw.mmio_writes);
            hw.clock_stuck=hw.reset_stuck=hw.mask_stuck=hw.disable_stuck=hw.gone=hw.clock_error=0;
            assert(!sunxi_mmc_stop(&host)&&!hw.registers[0x30/4]&&!hw.registers[0x04/4]);
        }
    } else if (scenario == 23) {
        assert(!start(&hw,&host));hw.cancel_host=&host;hw.cancel_word=16;
        assert(sunxi_mmc_transfer(&host,0,data,1,0)==-ENODEV&&hw.fifo_words==16&&!host.ready);
        assert(!sunxi_mmc_stop(&host));
    } else if (scenario == 24) {
        hw.gone=1;
        assert(start(&hw,&host)==-ENODEV&&!hw.mmio_writes);
        assert(sunxi_mmc_stop(&host)==-ENODEV&&!hw.mmio_writes);
    } else
        assert(!"unknown scenario");
    return 0;
}

#endif
