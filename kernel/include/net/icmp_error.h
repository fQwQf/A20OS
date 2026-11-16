#ifndef A20_NET_ICMP_ERROR_H
#define A20_NET_ICMP_ERROR_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Input has already passed the BSD IP/ICMP checksum and length checks. IPv4
 * raw sockets include the outer IP header; IPv6 raw sockets deliver ICMPv6.
 * Keep every quoted-header read bounded: ICMP may quote only a packet prefix. */
typedef struct net_icmp_error {
    uint8_t source[16], destination[16], source_port[2], destination_port[2];
    uint8_t type, code, origin;
    uint32_t error, info;
    size_t payload_offset, payload_len;
} net_icmp_error_t;

static inline uint16_t net_error_be16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static inline uint32_t net_error_be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static inline int net_icmp_error_parse(int family, const uint8_t *p, size_t n,
                                        net_icmp_error_t *out)
{
    net_icmp_error_t e = {0};
    size_t icmp = 0, quoted, udp;
    if (family == 2) {
        if (n < 20 || p[0] >> 4 != 4 || p[9] != 1) return 0;
        icmp = (p[0] & 15) * 4u;
        if (icmp < 20 || icmp > n) return 0;
    } else if (family != 10) return 0;
    if (n - icmp < 8) return 0;
    e.type = p[icmp]; e.code = p[icmp + 1];
    if (family == 2) {
        static const uint8_t unreachable[] = {
            101,113,92,111,90,95,101,112,64,101,113,101,113,113,113,113
        };
        e.origin = 2;
        if (e.type == 3 && e.code < sizeof(unreachable)) {
            e.error = unreachable[e.code];
            if (e.code == 4) e.info = net_error_be16(p + icmp + 6);
        } else if (e.type == 11 && e.code <= 1) e.error = 113;
        else if (e.type == 12 && e.code <= 2) e.error = 71;
        else return 0;
    } else {
        static const uint8_t unreachable[] = {101,13,113,113,111,13,13};
        e.origin = 3;
        if (e.type == 1 && e.code < sizeof(unreachable)) e.error = unreachable[e.code];
        else if (e.type == 2 && e.code == 0) {
            e.error = 90; e.info = net_error_be32(p + icmp + 4);
        } else if (e.type == 3 && e.code <= 1) e.error = 113;
        else if (e.type == 4 && e.code <= 2) e.error = 71;
        else return 0;
    }
    quoted = icmp + 8;
    if (family == 2) {
        if (n - quoted < 20 || p[quoted] >> 4 != 4 || p[quoted + 9] != 17)
            return 0;
        size_t ihl = (p[quoted] & 15) * 4u;
        if (ihl < 20 || ihl > n - quoted ||
            (net_error_be16(p + quoted + 6) & 0x1fff)) return 0;
        memcpy(e.source, p + quoted + 12, 4);
        memcpy(e.destination, p + quoted + 16, 4);
        udp = quoted + ihl;
    } else {
        if (n - quoted < 40 || p[quoted] >> 4 != 6) return 0;
        memcpy(e.source, p + quoted + 8, 16);
        memcpy(e.destination, p + quoted + 24, 16);
        uint8_t next = p[quoted + 6];
        udp = quoted + 40;
        for (unsigned i = 0; next != 17; i++) {
            if (i == 8 || n - udp < 8) return 0;
            size_t step;
            if (next == 0 || next == 43 || next == 60) step = (p[udp + 1] + 1u) * 8;
            else if (next == 51) step = (p[udp + 1] + 2u) * 4;
            else if (next == 44) {
                if (net_error_be16(p + udp + 2) & 0xfff8) return 0;
                step = 8;
            } else return 0;
            if (step > n - udp) return 0;
            next = p[udp]; udp += step;
        }
    }
    if (n - udp < 8) return 0;
    size_t length = net_error_be16(p + udp + 4);
    if (length < 8) return 0;
    memcpy(e.source_port, p + udp, 2);
    memcpy(e.destination_port, p + udp + 2, 2);
    e.payload_offset = udp + 8;
    e.payload_len = n - e.payload_offset;
    if (e.payload_len > length - 8) e.payload_len = length - 8;
    *out = e;
    return 1;
}
#endif
