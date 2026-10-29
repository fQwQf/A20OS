/* Pristine lwIP 2.2.1 clears IPv4 transit checksums before linkoutput. Complete
 * that software-offload request in the port when the NIC cannot do it. Never
 * edit upstream sources or try to derive a transport checksum from fragments. */
#include <stddef.h>
#include <stdint.h>

static uint32_t sum(const uint8_t *p, size_t n, uint32_t value) {
    while (n >= 2) { value += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) value += (uint32_t)*p << 8;
    return value;
}
static void finish(uint8_t *p, uint32_t value) {
    while (value >> 16) value = (value & 0xffff) + (value >> 16);
    value = (~value) & 0xffff;
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}
int a20_lwip_complete_software_tx(uint8_t *frame, size_t length) {
    if (length < 14 || frame[12] != 8 || frame[13] != 0) return 0;
    uint8_t *ip = frame + 14;
    if (length < 34 || ip[0] >> 4 != 4) return -1;
    size_t header = (ip[0] & 15) * 4;
    size_t total = ((size_t)ip[2] << 8) | ip[3];
    if (header < 20 || total < header || total > length - 14) return -1;
    if (ip[10] || ip[11]) return 0; /* Already completed by the stack. */
    unsigned fragments = (((unsigned)ip[6] << 8) | ip[7]) & 0x3fff;
    if (fragments) return -1; /* Incomplete offload cannot be completed safely. */
    uint8_t *l4 = ip + header;
    size_t payload = total - header;
    size_t offset;
    uint32_t pseudo = 0;
    if (ip[9] == 6 || ip[9] == 17) {
        offset = ip[9] == 6 ? 16 : 6;
        if (payload < (ip[9] == 6 ? 20u : 8u)) return -1;
        pseudo = sum(ip + 12, 8, (uint32_t)payload + ip[9]);
    } else if (ip[9] == 1) {
        offset = 2;
        if (payload < 8) return -1;
    } else {
        finish(ip + 10, sum(ip, header, 0));
        return 0;
    }
    l4[offset] = l4[offset + 1] = 0;
    finish(l4 + offset, sum(l4, payload, pseudo));
    if (ip[9] == 17 && !l4[offset] && !l4[offset + 1])
        l4[offset] = l4[offset + 1] = 255;
    finish(ip + 10, sum(ip, header, 0));
    return 0;
}
