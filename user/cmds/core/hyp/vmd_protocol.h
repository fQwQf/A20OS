#ifndef A20_VMD_PROTOCOL_H
#define A20_VMD_PROTOCOL_H
/* Wire protocol v1, little endian; all offsets are explicit. No C struct
 * layout, guest pointer, shell command or host descriptor crosses the wire. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define VMD_VERSION 1u
#define VMD_HEADER 24u
#define VMD_PAYLOAD 4096u
#define VMD_FRAME (VMD_HEADER + VMD_PAYLOAD)
#define VMD_NAME 32u
#define VMD_PATH 256u
#define VMD_START_SIZE (VMD_NAME + 4u + VMD_PATH)
#define VMD_RESPONSE 0x8000u
#define VMD_MAX_GUESTS 8u
#define VMD_MEMORY_MB 512u
#define VMD_HISTORY 65536u
#define VMD_INPUT 4096u
#define VMD_RECORD 64u
#define VMD_HELLO 1u
#define VMD_START 2u
#define VMD_LIST 3u
#define VMD_READ 4u
#define VMD_WRITE 5u
#define VMD_PAUSE 6u
#define VMD_RESUME 7u
#define VMD_STOP 8u
#define VMD_DESTROY 9u
#define VMD_STATS 10u
#define VMD_RUNNING 1u
#define VMD_PAUSED 2u
#define VMD_STOPPING 3u
#define VMD_EXITED 4u
#define VMD_PAUSING 5u
#define VMD_DRAINING 6u
static inline uint64_t vmd_get(const uint8_t *p, unsigned n)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < n; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static inline void vmd_put(uint8_t *p, uint64_t value, unsigned n)
{
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)(value >> (8 * i));
}
static inline uint32_t vmd_crc_step(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return crc;
}
static inline uint32_t vmd_checksum(const uint8_t *frame, size_t len)
{
    uint32_t crc = vmd_crc_step(~0u, frame, 20);
    return ~vmd_crc_step(crc, frame + VMD_HEADER, len);
}
static inline size_t vmd_frame(uint8_t *out, uint16_t op, uint64_t id,
                               const uint8_t *payload, size_t len)
{
    memcpy(out, "A2VM", 4);
    vmd_put(out + 4, VMD_VERSION, 2);
    vmd_put(out + 6, op, 2);
    vmd_put(out + 8, id, 8);
    vmd_put(out + 16, len, 4);
    if (len) memcpy(out + VMD_HEADER, payload, len);
    vmd_put(out + 20, vmd_checksum(out, len), 4);
    return VMD_HEADER + len;
}
#endif
