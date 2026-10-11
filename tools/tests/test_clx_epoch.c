#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/mcu/boot_epoch.h"

static uint8_t flash[128];
static int operations, cut_at, after;
static uint16_t read16(void *ctx, uint32_t at) {
    (void)ctx;
    assert(at < sizeof(flash) - 1 && !(at & 1));
    return flash[at] | ((uint16_t)flash[at + 1] << 8);
}
static int program16(void *ctx, uint32_t at, uint16_t value) {
    (void)ctx;
    assert(at < sizeof(flash) - 1 && !(at & 1) && read16(0, at) == 0xffff);
    int cut = ++operations == cut_at;
    if (cut && !after)
        return -1;
    flash[at] = (uint8_t)value;
    flash[at + 1] = (uint8_t)(value >> 8);
    return cut ? -1 : 0;
}
static int erase(void *ctx, uint32_t page) {
    (void)ctx;
    assert(page < 2);
    int cut = ++operations == cut_at;
    /* An interrupted erase may partially erase the inactive page. */
    memset(flash + page * 64, 0xff, cut ? 32 : 64);
    return cut ? -1 : 0;
}
int main(void) {
    a20_epoch_store_t store = {0, 64, read16, program16, erase};
    memset(flash, 0xff, sizeof(flash));
    uint64_t epoch = 0;
    for (unsigned i = 1; i <= 1000; i++) {
        assert(a20_epoch_next(&store, &epoch) == 0 && epoch == i);
    }
    uint8_t healthy[128];
    memcpy(healthy, flash, sizeof(flash));
    for (unsigned at = 0; at < sizeof(flash); at += 16) {
        if (a20_epoch_read(&store, at) == 1000) {
            flash[at] ^= 1;
            assert(a20_epoch_next(&store, &epoch) < 0);
            memcpy(flash, healthy, sizeof(flash));
            break;
        }
    }
    unsigned cuts = 0;
    /* Each position in multiple rotation states, both before and after a
     * half-word's commit, is a separate simulated power interruption. */
    for (unsigned initial = 1; initial <= 12; initial++) {
        memset(flash, 0xff, sizeof(flash));
        cut_at = 0;
        for (unsigned i = 0; i < initial; i++)
            assert(a20_epoch_next(&store, &epoch) == 0);
        uint8_t saved[128];
        memcpy(saved, flash, sizeof(flash));
        for (unsigned cut = 1; cut <= 9; cut++)
            for (after = 0; after <= 1; after++) {
                memcpy(flash, saved, sizeof(flash));
                operations = 0;
                cut_at = cut;
                uint64_t published = UINT64_MAX;
                int r = a20_epoch_next(&store, &published);
                cut_at = 0;
                if (r < 0) {
                    assert(published == UINT64_MAX);
                    assert(a20_epoch_next(&store, &epoch) == 0 &&
                           epoch > initial);
                } else {
                    assert(published == initial + 1);
                    assert(a20_epoch_next(&store, &epoch) == 0 &&
                           epoch > published);
                }
                cuts++;
            }
    }
    memset(flash, 0xff, sizeof(flash));
    flash[0] = 0;
    assert(a20_epoch_next(&store, &epoch) < 0); /* Corrupt nonblank store. */
    memset(flash, 0xff, sizeof(flash));
    for (unsigned i = 0; i < 4; i++)
        program16(0, i * 2, 0xffff);
    uint32_t crc = a20_epoch_checksum(UINT64_MAX);
    program16(0, 8, crc);
    program16(0, 10, crc >> 16);
    program16(0, 12, 0);
    program16(0, 14, A20_EPOCH_SEAL);
    assert(a20_epoch_next(&store, &epoch) < 0); /* No epoch wrap. */
    printf("test_clx_epoch: 1000 boots, %u power cuts, page rotation, "
           "corruption and exhaustion passed\n",
           cuts);
}
