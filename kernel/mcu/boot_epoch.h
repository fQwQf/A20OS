#ifndef A20_MCU_BOOT_EPOCH_H
#define A20_MCU_BOOT_EPOCH_H
#include <stdint.h>

/* Two-page append journal. The storage owner serializes access and supplies
 * atomic half-word programming. A new epoch becomes visible only after its
 * final seal and readback; page rotation never erases the highest committed
 * epoch. No heap, packed structs, or native-endian storage. */
#define A20_EPOCH_RECORD 16u
#define A20_EPOCH_SEAL 0xa20eu
typedef struct a20_epoch_store {
    void *ctx;
    uint32_t page_bytes;
    uint16_t (*read16)(void *ctx, uint32_t offset);
    int (*program16)(void *ctx, uint32_t offset, uint16_t value);
    int (*erase_page)(void *ctx, uint32_t page);
} a20_epoch_store_t;
static inline uint32_t a20_epoch_checksum(uint64_t epoch) {
    uint32_t crc = 0xa20e0001u;
    for (unsigned i = 0; i < 8; i++) {
        crc ^= (uint8_t)(epoch >> (i * 8));
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}
static inline uint64_t a20_epoch_read(const a20_epoch_store_t *s, uint32_t at) {
    if (s->read16(s->ctx, at + 14) != A20_EPOCH_SEAL ||
        s->read16(s->ctx, at + 12) != 0)
        return 0;
    uint64_t epoch = 0;
    for (unsigned i = 0; i < 4; i++)
        epoch |= (uint64_t)s->read16(s->ctx, at + i * 2) << (i * 16);
    uint32_t crc = s->read16(s->ctx, at + 8) |
                   ((uint32_t)s->read16(s->ctx, at + 10) << 16);
    return epoch && crc == a20_epoch_checksum(epoch) ? epoch : 0;
}
static inline int a20_epoch_empty(const a20_epoch_store_t *s, uint32_t at) {
    for (unsigned i = 0; i < A20_EPOCH_RECORD; i += 2)
        if (s->read16(s->ctx, at + i) != 0xffffu)
            return 0;
    return 1;
}
/* A nonblank store with no valid record is corruption, not a factory reset.
 * Refuse to reuse epoch 1. A deliberately erased store requires commissioning
 * under a new identity if peers might retain the previous identity/epoch. */
static inline int a20_epoch_next(const a20_epoch_store_t *s, uint64_t *out) {
    if (!s || !out || !s->read16 || !s->program16 || !s->erase_page ||
        s->page_bytes < A20_EPOCH_RECORD || s->page_bytes % A20_EPOCH_RECORD ||
        s->page_bytes > UINT32_MAX / 2)
        return -1;
    uint64_t highest = 0;
    uint32_t active = 0;
    int nonblank = 0;
    for (uint32_t at = 0; at < s->page_bytes * 2; at += A20_EPOCH_RECORD) {
        uint64_t epoch = a20_epoch_read(s, at);
        /* A damaged committed record might have been the greatest epoch.
         * Ignoring it could roll back to an older valid boot identity. */
        if (!epoch && s->read16(s->ctx, at + 14) == A20_EPOCH_SEAL)
            return -1;
        if (epoch > highest) {
            highest = epoch;
            active = at / s->page_bytes;
        }
        if (!a20_epoch_empty(s, at))
            nonblank = 1;
    }
    if ((!highest && nonblank) || highest == UINT64_MAX)
        return -1;
    uint32_t destination = UINT32_MAX;
    for (uint32_t at = active * s->page_bytes;
         at < (active + 1) * s->page_bytes; at += A20_EPOCH_RECORD) {
        if (a20_epoch_empty(s, at)) {
            destination = at;
            break;
        }
    }
    if (destination == UINT32_MAX) {
        uint32_t other = 1 - active;
        if (s->erase_page(s->ctx, other) < 0)
            return -1;
        destination = other * s->page_bytes;
        for (uint32_t at = destination; at < destination + s->page_bytes;
             at += A20_EPOCH_RECORD)
            if (!a20_epoch_empty(s, at))
                return -1;
    }
    uint64_t epoch = highest + 1;
    uint32_t crc = a20_epoch_checksum(epoch);
    /* Program data and checksum before the seal. A failed operation cannot
     * publish out; even a completed seal with a lost return is found on boot. */
    for (unsigned i = 0; i < 4; i++)
        if (s->program16(s->ctx, destination + i * 2,
                         (uint16_t)(epoch >> (i * 16))) < 0)
            return -1;
    if (s->program16(s->ctx, destination + 8, (uint16_t)crc) < 0 ||
        s->program16(s->ctx, destination + 10, (uint16_t)(crc >> 16)) < 0 ||
        s->program16(s->ctx, destination + 12, 0) < 0 ||
        s->program16(s->ctx, destination + 14, A20_EPOCH_SEAL) < 0 ||
        a20_epoch_read(s, destination) != epoch)
        return -1;
    *out = epoch;
    return 0;
}
#endif
