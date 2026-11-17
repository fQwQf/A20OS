/* PPPoE client. The kernel owns IP routing, NAT and fragmentation. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../crypto/md5.h"

enum {
    DISC = 0x8863, SESS = 0x8864, ETH_HEADER = 14, HEADER = 20, MTU = 1492,
    PADI = 9, PADO = 7, PADR = 0x19, PADS = 0x65, PADT = 0xa7,
    SERVICE = 0x101, AC_NAME = 0x102, HOST_UNIQ = 0x103, COOKIE = 0x104,
    RELAY = 0x110,
    LCP = 0xc021, PAP = 0xc023, CHAP = 0xc223, IPCP = 0x8021, IPV4 = 0x21,
    IPV6CP = 0x8057, IPV6 = 0x57, IID = 1, IPCP_ADDRESS = 3,
    DNS_PRIMARY = 129, DNS_SECONDARY = 131,
    REQUEST = 1, ACK = 2, NAK = 3, REJECT = 4, TERMINATE = 5,
    TERMINATE_ACK = 6, CODE_REJECT = 7, PROTOCOL_REJECT = 8, ECHO = 9, ECHO_REPLY = 10,
};

struct config {
    const char *wan, *tun, *service;
    uint16_t auth;
    bool ipv6;
    const char *dns_file;
    char user[256], password[256];
};

struct session {
    int disc, data, tun;
    struct sockaddr_ll link;
    uint8_t mac[6], peer[6], token[8], magic[4], address[4], remote[4];
    uint8_t iid[8], remote_iid[8], suggested_iid[8], dns[2][4];
    uint16_t id, mtu, mru, auth;
    uint8_t next_id, chap_id, echo_id;
    uint8_t peer_lcp[15], peer_lcp_len;
    bool chap_pending, echo_pending;
    unsigned missed;
    int64_t echo_at, auth_at;
    struct stat dns_owner;
    bool dns_written;
};

struct packet {
    uint8_t bytes[1514];
    size_t len;
    uint16_t protocol;
    uint8_t code;
};

static volatile sig_atomic_t stopping;
static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};

static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static void erase(void *data, size_t size)
{
    volatile uint8_t *p = data;
    while (size--)
        *p++ = 0;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = value >> 8;
    p[1] = value;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct ifreq ifreq_for(const char *name)
{
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    return ifr;
}

static int packet_socket(struct sockaddr_ll link, uint16_t protocol)
{
    link.sll_protocol = htons(protocol);
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, link.sll_protocol);
    if (fd >= 0 && bind(fd, (void *)&link, sizeof(link)) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int frame_send(struct session *s, uint16_t type, const uint8_t mac[6],
                      uint8_t code, uint16_t sid, const uint8_t *payload, size_t len)
{
    uint8_t frame[1514];
    if (len > sizeof(frame) - HEADER)
        return -EMSGSIZE;
    memcpy(frame, mac, 6);
    memcpy(frame + 6, s->mac, 6);
    put16(frame + 12, type);
    frame[14] = 0x11;
    frame[15] = code;
    put16(frame + 16, sid);
    put16(frame + 18, (uint16_t)len);
    memcpy(frame + HEADER, payload, len);
    size_t wire_len = HEADER + len;
    if (wire_len < 60) {
        memset(frame + wire_len, 0, 60 - wire_len);
        wire_len = 60;
    }
    struct sockaddr_ll link = s->link;
    link.sll_protocol = htons(type);
    memcpy(link.sll_addr, mac, 6);
    int fd = type == DISC ? s->disc : s->data;
    ssize_t sent = sendto(fd, frame, wire_len, 0, (void *)&link, sizeof(link));
    int rc = sent == (ssize_t)wire_len ? 0 : sent < 0 ? -errno : -EIO;
    erase(frame + HEADER, len);
    return rc;
}

static int read_frame(struct session *s, int fd, struct packet *p)
{
    ssize_t n = recv(fd, p->bytes, sizeof(p->bytes), 0);
    if (n < 0)
        return -errno;
    uint8_t *b = p->bytes;
    if (n < HEADER || get16(b + 12) != (fd == s->disc ? DISC : SESS) ||
        b[14] != 0x11 || memcmp(b, s->mac, 6) ||
        (b[6] & 1) || !memcmp(b + 6, s->mac, 6))
        return 0;
    p->len = get16(b + 18);
    p->code = b[15];
    return p->len <= (size_t)n - HEADER ? 1 : 0;
}

struct tag {
    const uint8_t *data;
    size_t len;
};

struct offer {
    struct tag service, ac, host, cookie, relay;
};

static bool parse_tags(const uint8_t *p, size_t len, struct offer *offer)
{
    while (len >= 4) {
        uint16_t type = get16(p), size = get16(p + 2);
        p += 4;
        len -= 4;
        if (size > len)
            return false;
        if (type == 0)
            return size == 0 && len == 0;
        if (type >= 0x201 && type <= 0x203)
            return false;
        struct tag *tag = type == SERVICE ? &offer->service :
                          type == AC_NAME ? &offer->ac :
                          type == HOST_UNIQ ? &offer->host :
                          type == COOKIE ? &offer->cookie :
                          type == RELAY ? &offer->relay : NULL;
        if (tag) {
            if (tag->data)
                return false;
            *tag = (struct tag){p, size};
        }
        p += size;
        len -= size;
    }
    return len == 0;
}

static int append_tag(uint8_t *payload, size_t *len, uint16_t type, struct tag value)
{
    if (value.len + 4 > 1494 - *len)
        return -EMSGSIZE;
    put16(payload + *len, type);
    put16(payload + *len + 2, (uint16_t)value.len);
    memcpy(payload + *len + 4, value.data, value.len);
    *len += 4 + value.len;
    return 0;
}

static int wait_fd(int fd, int64_t deadline)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int64_t left = deadline - now_ms();
    if (left <= 0 || stopping)
        return 0;
    int rc = poll(&p, 1, (int)left);
    if (rc < 0)
        return errno == EINTR ? 0 : -errno;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
        return -EIO;
    return rc;
}

static int discover(struct session *s, const char *service)
{
    uint8_t data[1494];
    size_t len = 0;
    struct tag name = {(const uint8_t *)service, strlen(service)};
    struct tag token = {s->token, sizeof(s->token)};
    append_tag(data, &len, SERVICE, name);
    append_tag(data, &len, HOST_UNIQ, token);
    unsigned attempt = 0;
    while (attempt < 4 && !stopping) {
        bool request = memcmp(s->peer, broadcast, 6) != 0;
        int rc = frame_send(s, DISC, s->peer, request ? PADR : PADI, 0, data, len);
        if (rc < 0)
            return rc;
        int64_t deadline = now_ms() + (1000 << attempt++);
        while ((rc = wait_fd(s->disc, deadline)) > 0) {
            struct packet p;
            rc = read_frame(s, s->disc, &p);
            if (rc < 0)
                return rc;
            if (!rc || p.code != (request ? PADS : PADO) ||
                (request && memcmp(p.bytes + 6, s->peer, 6)))
                continue;
            struct offer offer = {0};
            if (!parse_tags(p.bytes + HEADER, p.len, &offer) ||
                !offer.host.data || offer.host.len != token.len ||
                memcmp(offer.host.data, token.data, token.len) || !offer.service.data ||
                (name.len && (offer.service.len != name.len ||
                              memcmp(offer.service.data, name.data, name.len))))
                continue;
            uint16_t sid = get16(p.bytes + 16);
            if (request) {
                size_t service_len = get16(data + 2);
                if (sid && sid != 0xffff && offer.service.len == service_len &&
                    !memcmp(offer.service.data, data + 4, service_len)) {
                    s->id = sid;
                    return 0;
                }
                continue;
            }
            if (sid || !offer.ac.data)
                continue;
            len = 0;
            if (append_tag(data, &len, SERVICE, offer.service) ||
                append_tag(data, &len, HOST_UNIQ, token) ||
                (offer.cookie.data && append_tag(data, &len, COOKIE, offer.cookie)) ||
                (offer.relay.data && append_tag(data, &len, RELAY, offer.relay)))
                return -EMSGSIZE;
            memcpy(s->peer, p.bytes + 6, 6);
            /* Start PADR immediately with a fresh retransmission budget. */
            attempt = 0;
            break;
        }
        if (rc < 0)
            return rc;
    }
    return -ETIMEDOUT;
}

static int ppp_send(struct session *s, uint16_t protocol, const uint8_t *p, size_t len)
{
    uint8_t payload[MTU + 2];
    if (len > s->mtu || len > MTU)
        return -EMSGSIZE;
    put16(payload, protocol);
    memcpy(payload + 2, p, len);
    int rc = frame_send(s, SESS, s->peer, 0, s->id, payload, len + 2);
    erase(payload + 2, len);
    return rc;
}

static int control(struct session *s, uint16_t protocol, uint8_t code,
                   uint8_t id, const uint8_t *data, size_t len)
{
    uint8_t payload[MTU];
    if (len > sizeof(payload) - 4)
        return -EMSGSIZE;
    payload[0] = code;
    payload[1] = id;
    put16(payload + 2, (uint16_t)(len + 4));
    memcpy(payload + 4, data, len);
    int rc = ppp_send(s, protocol, payload, len + 4);
    erase(payload + 4, len);
    return rc;
}

/* Return a session packet, an online TUN event (2), or an error/timeout. */
static int receive(struct session *s, int64_t deadline, struct packet *p)
{
    while (!stopping) {
        int64_t left = deadline - now_ms();
        if (left <= 0)
            break;
        struct pollfd fds[] = {
            {.fd = s->disc, .events = POLLIN}, {.fd = s->data, .events = POLLIN},
            {.fd = s->tun, .events = POLLIN},
        };
        int rc = poll(fds, 3, (int)left);
        if (rc <= 0)
            return rc < 0 && errno != EINTR ? -errno : 0;
        for (unsigned i = 0; i < 3; ++i)
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                return -EIO;
        if (fds[0].revents & POLLIN) {
            rc = read_frame(s, s->disc, p);
            if (rc < 0)
                return rc;
            if (rc && p->code == PADT && get16(p->bytes + 16) == s->id &&
                !memcmp(p->bytes + 6, s->peer, 6))
                return -ECONNRESET;
        }
        if (fds[1].revents & POLLIN) {
            rc = read_frame(s, s->data, p);
            if (rc < 0)
                return rc;
            if (rc && p->code == 0 && get16(p->bytes + 16) == s->id &&
                !memcmp(p->bytes + 6, s->peer, 6) && p->len >= 2 &&
                p->len <= (size_t)s->mru + 2) {
                p->protocol = get16(p->bytes + HEADER);
                p->len -= 2;
                return 1;
            }
        }
        if (fds[2].revents & POLLIN)
            return 2;
    }
    return 0;
}

static bool control_packet(const struct packet *p, size_t *len)
{
    if (p->len < 4)
        return false;
    *len = get16(p->bytes + HEADER + 4);
    return *len >= 4 && *len <= p->len;
}

static bool valid_address(const uint8_t ip[4])
{
    return ip[0] != 0 && ip[0] != 127 && ip[0] < 224;
}

static bool valid_iid(const uint8_t iid[8])
{
    return memcmp(iid, "\0\0\0\0\0\0\0\0", 8) != 0;
}

static void suggest_peer_iid(const uint8_t local[8], uint8_t opt[10])
{
    opt[0] = IID;
    opt[1] = 10;
    memcpy(opt + 2, local, 8);
    opt[2] &= (uint8_t)~2;
    opt[9] ^= 1;
    if (!valid_iid(opt + 2))
        opt[9] = 2;
}

static int peer_request(struct session *s, const struct config *cfg, uint16_t protocol,
                        const uint8_t *p, size_t len, bool *acked)
{
    uint8_t rejected[MTU], nak[10];
    size_t rejected_len = 0, nak_len = 0;
    uint16_t peer_mtu = MTU, auth = 0;
    uint8_t peer_ip[4] = {0}, peer_iid[8] = {0};
    bool seen[256] = {0};
    for (size_t off = 4; off < len;) {
        if (len - off < 2 || p[off + 1] < 2 || p[off + 1] > len - off)
            return 0;
        const uint8_t *opt = p + off;
        uint8_t type = opt[0], n = opt[1];
        bool ok = false;
        if (!seen[type] && protocol == LCP) {
            if (type == 1 && n == 4) {
                peer_mtu = get16(opt + 2);
                ok = true;
                if (peer_mtu < 128 || peer_mtu > MTU) {
                    nak[0] = 1;
                    nak[1] = 4;
                    put16(nak + 2, MTU);
                    nak_len = 4;
                }
            } else if (type == 3 && n >= 4) {
                auth = get16(opt + 2);
                ok = auth == cfg->auth &&
                     ((auth == PAP && n == 4) || (auth == CHAP && n == 5 && opt[4] == 5));
                if (!ok)
                    return -EACCES;
            } else if (type == 5 && n == 6) {
                ok = memcmp(opt + 2, s->magic, 4) != 0;
            }
        } else if (!seen[type] && protocol == IPCP && type == IPCP_ADDRESS && n == 6) {
            memcpy(peer_ip, opt + 2, 4);
            ok = valid_address(peer_ip);
        } else if (!seen[type] && protocol == IPV6CP && type == IID && n == 10) {
            memcpy(peer_iid, opt + 2, 8);
            ok = true;
        }
        seen[type] = true;
        if (!ok) {
            memcpy(rejected + rejected_len, opt, n);
            rejected_len += n;
        }
        off += n;
    }
    *acked = false;
    if (rejected_len)
        return control(s, protocol, REJECT, p[1], rejected, rejected_len);
    if (protocol == IPV6CP &&
        (!valid_iid(peer_iid) || !memcmp(peer_iid, s->iid, 8))) {
        suggest_peer_iid(s->iid, nak);
        if (!rejected_len)
            memcpy(s->suggested_iid, nak + 2, 8);
        nak_len = 10;
    }
    if (nak_len)
        return control(s, protocol, NAK, p[1], nak, nak_len);
    if (protocol == LCP) {
        if (auth != cfg->auth)
            return -EACCES;
        if (len - 4 > sizeof(s->peer_lcp))
            return -EPROTO;
        memcpy(s->peer_lcp, p + 4, len - 4);
        s->peer_lcp_len = (uint8_t)(len - 4);
        s->auth = auth;
        s->mtu = peer_mtu;
    } else if (protocol == IPCP) {
        memcpy(s->remote, peer_ip, 4);
    } else {
        memcpy(s->remote_iid, peer_iid, 8);
    }
    *acked = true;
    return control(s, protocol, ACK, p[1], p + 4, len - 4);
}

static int lcp_event(struct session *s, const uint8_t *p, size_t len)
{
    if (p[0] == TERMINATE) {
        (void)control(s, LCP, TERMINATE_ACK, p[1], p, 0);
        return -ECONNRESET;
    }
    if (p[0] == ECHO && len >= 8) {
        uint8_t data[MTU];
        memcpy(data, s->magic, 4);
        memcpy(data + 4, p + 8, len - 8);
        return control(s, LCP, ECHO_REPLY, p[1], data, len - 4);
    }
    if (p[0] == ECHO_REPLY && len >= 8 && s->echo_pending && p[1] == s->echo_id &&
        (!memcmp(s->magic, "\0\0\0\0", 4) || memcmp(p + 4, s->magic, 4))) {
        s->echo_pending = false;
        s->missed = 0;
    }
    if (p[0] == REQUEST) {
        if (len != (size_t)s->peer_lcp_len + 4 ||
            memcmp(s->peer_lcp, p + 4, len - 4))
            return -ECONNRESET;
        return control(s, LCP, ACK, p[1], p + 4, len - 4);
    }
    return 0;
}

/* A repeated identical request may mean our ACK was lost. A changed request
 * tears down the installed network configuration and redials. */
static int ncp_open_event(struct session *s, uint16_t protocol,
                          const uint8_t *p, size_t len)
{
    if (p[0] == TERMINATE) {
        (void)control(s, protocol, TERMINATE_ACK, p[1], p, 0);
        return -ECONNRESET;
    }
    if (p[0] != REQUEST)
        return 0;
    bool same = protocol == IPCP ?
        ((len == 10 && p[4] == IPCP_ADDRESS && p[5] == 6 && !memcmp(p + 6, s->remote, 4)) ||
         (len == 4 && !valid_address(s->remote))) :
        (len == 14 && p[4] == IID && p[5] == 10 && !memcmp(p + 6, s->remote_iid, 8));
    return same ? control(s, protocol, ACK, p[1], p + 4, len - 4) : -ECONNRESET;
}

/* IPCP NAKs may supply several independent addresses. Validate the whole
 * response before applying it. A reject may remove DNS options only. */
static int ipcp_response(uint8_t opts[18], size_t *size, const uint8_t *data,
                         size_t length, bool reject)
{
    if (!length || length % 6)
        return 0;
    uint8_t next[18];
    memcpy(next, opts, *size);
    unsigned selected = 0;
    for (size_t i = 0; i < length; i += 6) {
        size_t j;
        for (j = 0; j < *size; j += 6)
            if (opts[j] == data[i])
                break;
        if (j == *size || data[i + 1] != 6 || selected & (1u << (j / 6)))
            return 0;
        selected |= 1u << (j / 6);
        if (reject) {
            if (memcmp(opts + j, data + i, 6))
                return 0;
        } else {
            if (!valid_address(data + i + 2))
                return 0;
            memcpy(next + j + 2, data + i + 2, 4);
        }
    }
    if (reject) {
        if (selected & 1) /* The address is always the first option. */
            return -EPROTONOSUPPORT;
        size_t out = 0;
        for (size_t j = 0; j < *size; j += 6) {
            if (!(selected & (1u << (j / 6)))) {
                memmove(next + out, next + j, 6);
                out += 6;
            }
        }
        *size = out;
    }
    memcpy(opts, next, *size);
    return 1;
}

static int negotiate(struct session *s, const struct config *cfg, uint16_t protocol)
{
    uint8_t opts[18] = {1, 4, MTU >> 8, MTU & 255, 5, 6};
    size_t size = 10;
    memcpy(opts + 6, s->magic, 4);
    if (protocol == IPCP) {
        opts[0] = IPCP_ADDRESS;
        opts[1] = 6;
        memcpy(opts + 2, s->address, 4);
        size = 6;
        if (cfg->dns_file) {
            opts[6] = DNS_PRIMARY;
            opts[7] = 6;
            memset(opts + 8, 0, 4);
            opts[12] = DNS_SECONDARY;
            opts[13] = 6;
            size = 18;
        }
    } else if (protocol == IPV6CP) {
        opts[0] = IID;
        opts[1] = 10;
        memcpy(opts + 2, s->iid, 8);
    }
    bool ours = false, theirs = false;
    for (unsigned retry = 0; retry < 10 && !stopping; ++retry) {
        uint8_t id = ++s->next_id;
        int rc = control(s, protocol, REQUEST, id, opts, size);
        if (rc < 0)
            return rc;
        ours = false;
        int64_t until = now_ms() + 1500;
        struct packet packet;
        while ((rc = receive(s, until, &packet)) > 0) {
            size_t len;
            if (!control_packet(&packet, &len))
                continue;
            uint8_t *p = packet.bytes + HEADER + 2;
            if (packet.protocol != protocol) {
                if (packet.protocol == LCP) {
                    if (p[0] == PROTOCOL_REJECT && len >= 6 && get16(p + 4) == protocol)
                        return -EPROTONOSUPPORT;
                    if ((rc = lcp_event(s, p, len)) < 0)
                        return rc;
                } else if (protocol == IPV6CP && packet.protocol == IPCP &&
                           (rc = ncp_open_event(s, IPCP, p, len)) < 0) {
                    return rc;
                }
                continue;
            }
            if (p[0] == REQUEST) {
                rc = peer_request(s, cfg, protocol, p, len, &theirs);
                if (rc < 0)
                    return rc;
            } else if (p[0] == TERMINATE) {
                return protocol == LCP ? lcp_event(s, p, len) : ncp_open_event(s, protocol, p, len);
            } else if (p[1] == id) {
                if (p[0] == ACK && len == size + 4 && !memcmp(p + 4, opts, size)) {
                    if (protocol == IPCP) {
                        if (!valid_address(opts + 2))
                            return -EADDRNOTAVAIL;
                        memcpy(s->address, opts + 2, 4);
                        memset(s->dns, 0, sizeof(s->dns));
                        for (size_t j = 6; j < size; j += 6)
                            memcpy(s->dns[opts[j] == DNS_SECONDARY], opts + j + 2, 4);
                    }
                    ours = true;
                } else if (protocol == IPCP && (p[0] == NAK || p[0] == REJECT)) {
                    rc = ipcp_response(opts, &size, p + 4, len - 4, p[0] == REJECT);
                    if (rc < 0)
                        return rc;
                    if (rc)
                        break;
                } else if (protocol == IPV6CP && p[0] == NAK && len == 14 &&
                           p[4] == IID && p[5] == 10 && valid_iid(p + 6)) {
                    if (!memcmp(s->remote_iid, p + 6, 8) ||
                        !memcmp(s->suggested_iid, p + 6, 8)) {
                        if (getrandom(s->iid, sizeof(s->iid), 0) != sizeof(s->iid))
                            return -EIO;
                        s->iid[0] &= (uint8_t)~2;
                        if (!valid_iid(s->iid))
                            s->iid[7] = 1;
                    } else {
                        memcpy(s->iid, p + 6, 8);
                    }
                    memcpy(opts + 2, s->iid, 8);
                    if (!memcmp(s->iid, s->remote_iid, 8))
                        theirs = false;
                    break;
                } else if (protocol == LCP && p[0] == NAK && len == 8 &&
                           p[4] == 1 && p[5] == 4 &&
                           get16(p + 6) >= 128 && get16(p + 6) <= MTU) {
                    s->mru = get16(p + 6);
                    put16(opts + 2, s->mru);
                    break;
                } else if (p[0] == REJECT) {
                    /* Both peers may omit the magic number. */
                    if (protocol != LCP || size != 10 || len != 10 ||
                        memcmp(p + 4, opts + 4, 6))
                        return -EPROTONOSUPPORT;
                    size = 4;
                    memset(s->magic, 0, sizeof(s->magic));
                    break;
                }
            }
            if (ours && theirs)
                return 0;
        }
        if (rc < 0)
            return rc;
    }
    return -ETIMEDOUT;
}

static int chap_event(struct session *s, const struct config *cfg,
                      const uint8_t *p, size_t len, bool *success)
{
    *success = false;
    if (p[0] == 1 && len > 5 && p[4] && p[4] <= len - 5) {
        struct md5 hash;
        uint8_t response[1 + MD5_DIGEST_LENGTH + 255];
        response[0] = MD5_DIGEST_LENGTH;
        md5_init(&hash);
        md5_update(&hash, p + 1, 1);
        md5_update(&hash, cfg->password, strlen(cfg->password));
        md5_update(&hash, p + 5, p[4]);
        md5_sum(&hash, response + 1);
        erase(&hash, sizeof(hash));
        size_t user_len = strlen(cfg->user);
        memcpy(response + 1 + MD5_DIGEST_LENGTH, cfg->user, user_len);
        s->chap_pending = true;
        s->chap_id = p[1];
        s->auth_at = now_ms() + 15000;
        int rc = control(s, CHAP, 2, p[1], response, 1 + MD5_DIGEST_LENGTH + user_len);
        erase(response, sizeof(response));
        return rc;
    }
    if (s->chap_pending && p[1] == s->chap_id && (p[0] == 3 || p[0] == 4)) {
        s->chap_pending = false;
        if (p[0] == 4)
            return -EACCES;
        *success = true;
    }
    return 0;
}

static int pap_request(struct session *s, const struct config *cfg, uint8_t id)
{
    size_t user = strlen(cfg->user), password = strlen(cfg->password);
    uint8_t data[512];
    data[0] = (uint8_t)user;
    memcpy(data + 1, cfg->user, user);
    data[1 + user] = (uint8_t)password;
    memcpy(data + 2 + user, cfg->password, password);
    int rc = control(s, PAP, 1, id, data, 2 + user + password);
    erase(data, sizeof(data));
    return rc;
}

static int authenticate(struct session *s, const struct config *cfg)
{
    uint8_t id = ++s->next_id;
    for (unsigned retry = 0; retry < 5 && !stopping; ++retry) {
        int rc = 0;
        if (s->auth == PAP && (rc = pap_request(s, cfg, id)) < 0)
            return rc;
        struct packet packet;
        int64_t until = now_ms() + 3000;
        while ((rc = receive(s, until, &packet)) > 0) {
            size_t len;
            if (!control_packet(&packet, &len))
                continue;
            const uint8_t *p = packet.bytes + HEADER + 2;
            if (packet.protocol == LCP) {
                if ((rc = lcp_event(s, p, len)) < 0)
                    return rc;
            } else if (s->auth == PAP && packet.protocol == PAP && p[1] == id &&
                       len >= 5 && p[4] == len - 5) {
                if (p[0] == ACK)
                    return 0;
                if (p[0] == NAK)
                    return -EACCES;
            } else if (s->auth == CHAP && packet.protocol == CHAP) {
                bool success;
                rc = chap_event(s, cfg, p, len, &success);
                if (rc < 0 || success)
                    return rc;
            }
        }
        if (rc < 0)
            return rc;
    }
    return -ETIMEDOUT;
}

static int netlink_ack(const void *request, size_t size)
{
    enum { NL_ERROR = 2 };
    struct { uint16_t family, pad; uint32_t pid, groups; } local = {.family = AF_NETLINK};
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    int rc;
    if (bind(fd, (void *)&local, sizeof(local)) < 0 ||
        send(fd, request, size, 0) < 0) {
        rc = -errno;
    } else {
        rc = wait_fd(fd, now_ms() + 2000);
        if (!rc)
            rc = -ETIMEDOUT;
        if (rc > 0) {
            uint8_t ack[128];
            ssize_t len = recv(fd, ack, sizeof(ack), 0);
            uint16_t type;
            uint32_t seq;
            int32_t error;
            rc = -EPROTO;
            if (len >= 20) {
                memcpy(&type, ack + 4, 2);
                memcpy(&seq, ack + 8, 4);
                memcpy(&error, ack + 16, 4);
                if (type == NL_ERROR && seq == 1 && error <= 0)
                    rc = error;
            }
        }
    }
    close(fd);
    return rc;
}

static int default_route(unsigned index, int family)
{
    /* Linux RTM_NEWROUTE with RTA_OIF. Use an exclusive add so an
     * existing uplink route belongs to its original owner. */
    enum { NEW_ROUTE = 24, MAIN_TABLE = 254, OUT_IF = 4,
           REQUEST_ACK_CREATE_EXCL = 1 | 4 | 0x400 | 0x200 };
    struct {
        uint32_t len;
        uint16_t type, flags;
        uint32_t seq, pid;
        uint8_t family, dst_len, src_len, tos, table, protocol, scope, kind;
        uint32_t route_flags;
        uint16_t attr_len, attr_type;
        uint32_t ifindex;
    } req = {
        .len = sizeof(req), .type = NEW_ROUTE, .flags = REQUEST_ACK_CREATE_EXCL,
        .seq = 1, .family = family, .table = MAIN_TABLE, .protocol = 4, .scope = 253,
        .kind = 1, .attr_len = 8, .attr_type = OUT_IF, .ifindex = index,
    };
    return netlink_ack(&req, sizeof(req));
}

static int set_link_local(unsigned index, const uint8_t iid[8])
{
    enum { NEW_ADDRESS = 20, LOCAL = 2, REQUEST_ACK_CREATE_EXCL = 1 | 4 | 0x400 | 0x200 };
    struct {
        uint32_t len;
        uint16_t type, flags;
        uint32_t seq, pid;
        uint8_t family, prefix, address_flags, scope;
        uint32_t ifindex;
        uint16_t attr_len, attr_type;
        uint8_t address[16];
    } req = {
        .len = sizeof(req), .type = NEW_ADDRESS, .flags = REQUEST_ACK_CREATE_EXCL,
        .seq = 1, .family = AF_INET6, .prefix = 64, .scope = 253,
        .ifindex = index, .attr_len = 20, .attr_type = LOCAL,
        .address = {0xfe, 0x80},
    };
    memcpy(req.address + 8, iid, 8);
    return netlink_ack(&req, sizeof(req));
}

static int set_address(int fd, const char *name, unsigned long op, const uint8_t ip[4])
{
    struct ifreq ifr = ifreq_for(name);
    struct sockaddr_in addr = {.sin_family = AF_INET};
    memcpy(&addr.sin_addr, ip, 4);
    memcpy(&ifr.ifr_addr, &addr, sizeof(addr));
    return ioctl(fd, op, &ifr) < 0 ? -errno : 0;
}

static int open_tun(struct session *s, const struct config *cfg, int ctl)
{
    s->tun = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (s->tun < 0)
        return -errno;
    struct ifreq ifr = ifreq_for(cfg->tun);
    ifr.ifr_flags = 1 | 0x1000; /* IFF_TUN | IFF_NO_PI */
    if (ioctl(s->tun, _IOW('T', 202, int), &ifr) < 0)
        return -errno;
    const uint8_t mask[] = {255, 255, 255, 255};
    int rc = set_address(ctl, cfg->tun, SIOCSIFNETMASK, mask);
    if (rc < 0)
        return rc;
    rc = set_address(ctl, cfg->tun, SIOCSIFADDR, s->address);
    if (rc < 0)
        return rc;
    ifr = ifreq_for(cfg->tun);
    ifr.ifr_mtu = s->mtu;
    if (ioctl(ctl, SIOCSIFMTU, &ifr) < 0 || ioctl(ctl, SIOCGIFFLAGS, &ifr) < 0)
        return -errno;
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(ctl, SIOCSIFFLAGS, &ifr) < 0)
        return -errno;
    unsigned index = if_nametoindex(cfg->tun);
    if (!index)
        return -ENODEV;
    if ((rc = default_route(index, AF_INET)) < 0 || !cfg->ipv6)
        return rc;
    if ((rc = set_link_local(index, s->iid)) < 0)
        return rc;
    return default_route(index, AF_INET6);
}

static bool valid_datagram(uint16_t protocol, const uint8_t *p, size_t len)
{
    return protocol == IPV4 ?
        (len >= 20 && p[0] >> 4 == 4 && get16(p + 2) == len) :
        (len >= 40 && p[0] >> 4 == 6 && (size_t)get16(p + 4) + 40 == len);
}

static int online(struct session *s, const struct config *cfg)
{
    s->echo_at = now_ms() + 5000;
    while (!stopping) {
        int64_t now = now_ms();
        if (s->chap_pending && now >= s->auth_at)
            return -ETIMEDOUT;
        if (now >= s->echo_at) {
            if (s->echo_pending && ++s->missed >= 3)
                return -ETIMEDOUT;
            s->echo_id = ++s->next_id;
            int rc = control(s, LCP, ECHO, s->echo_id, s->magic, sizeof(s->magic));
            if (rc < 0)
                return rc;
            s->echo_pending = true;
            s->echo_at = now + 5000;
        }
        int64_t until = s->echo_at;
        if (s->chap_pending && s->auth_at < until)
            until = s->auth_at;
        struct packet packet;
        int rc = receive(s, until, &packet);
        if (rc < 0)
            return rc;
        if (rc == 2) {
            ssize_t n = read(s->tun, packet.bytes, sizeof(packet.bytes));
            if (n < 0 && errno != EAGAIN && errno != EINTR)
                return -errno;
            uint16_t proto = n > 0 && packet.bytes[0] >> 4 == 6 ? IPV6 : IPV4;
            if (n > 0 && n <= s->mtu && (proto == IPV4 || cfg->ipv6) &&
                valid_datagram(proto, packet.bytes, n)) {
                rc = ppp_send(s, proto, packet.bytes, (size_t)n);
                if (rc < 0 && rc != -ENOBUFS && rc != -EAGAIN)
                    return rc;
            }
            continue;
        }
        if (!rc)
            continue;
        const uint8_t *p = packet.bytes + HEADER + 2;
        if (packet.protocol == IPV4 || (cfg->ipv6 && packet.protocol == IPV6)) {
            if (valid_datagram(packet.protocol, p, packet.len)) {
                ssize_t n = write(s->tun, p, packet.len);
                if (n < 0 && errno != EAGAIN && errno != ENOBUFS && errno != EINTR)
                    return -errno;
            }
            continue;
        }
        size_t len;
        if (packet.protocol == LCP && control_packet(&packet, &len)) {
            rc = lcp_event(s, p, len);
        } else if (packet.protocol == CHAP && s->auth == CHAP && control_packet(&packet, &len)) {
            bool success;
            rc = chap_event(s, cfg, p, len, &success);
        } else if (packet.protocol == IPCP || (cfg->ipv6 && packet.protocol == IPV6CP)) {
            if (control_packet(&packet, &len))
                rc = ncp_open_event(s, packet.protocol, p, len);
        } else if (packet.protocol != PAP && packet.protocol != CHAP &&
                   packet.protocol != LCP) {
            /* Reject an unimplemented network protocol without resetting IPv4. */
            size_t reject_len = packet.len + 2;
            if (reject_len > (size_t)s->mtu - 4)
                reject_len = s->mtu - 4;
            rc = control(s, LCP, PROTOCOL_REJECT, ++s->next_id,
                         packet.bytes + HEADER, reject_len);
        }
        if (rc < 0)
            return rc;
    }
    return 0;
}

/* Publish a complete per-link resolv.conf. The caller supplies a path
 * reserved for this link, and the exclusive link never replaces a file. */
static int publish_dns(struct session *s, const char *path)
{
    if (!path || (!valid_address(s->dns[0]) && !valid_address(s->dns[1])))
        return 0;
    char temporary[PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof(temporary))
        return -ENAMETOOLONG;
    int fd = mkstemp(temporary);
    if (fd < 0)
        return -errno;
    FILE *output = fdopen(fd, "w");
    if (!output) {
        int error = -errno;
        close(fd);
        unlink(temporary);
        return error;
    }
    int rc = 0;
    for (unsigned i = 0; i < 2 && !rc; ++i) {
        if (valid_address(s->dns[i])) {
            char addr[INET_ADDRSTRLEN];
            if (!inet_ntop(AF_INET, s->dns[i], addr, sizeof(addr)) ||
                fprintf(output, "nameserver %s\n", addr) < 0)
                rc = -errno;
        }
    }
    if (!rc && (fchmod(fd, 0644) < 0 || fstat(fd, &s->dns_owner) < 0))
        rc = -errno;
    if (fclose(output) < 0 && !rc)
        rc = -errno;
    if (!rc) {
        if (link(temporary, path) < 0)
            rc = -errno;
        else
            s->dns_written = true;
    }
    unlink(temporary);
    return rc;
}

static void remove_dns(struct session *s, const char *path)
{
    struct stat status;
    if (s->dns_written && lstat(path, &status) == 0 &&
        status.st_dev == s->dns_owner.st_dev && status.st_ino == s->dns_owner.st_ino)
        (void)unlink(path);
}

static int dial(struct session *s, const struct config *cfg, int ctl)
{
    s->link.sll_ifindex = (int)if_nametoindex(cfg->wan);
    if (!s->link.sll_ifindex)
        return -ENODEV;
    struct ifreq ifr = ifreq_for(cfg->wan);
    if (ioctl(ctl, SIOCGIFHWADDR, &ifr) < 0)
        return -errno;
    memcpy(s->mac, ifr.ifr_hwaddr.sa_data, 6);
    memcpy(s->iid, s->mac, 3);
    s->iid[0] ^= 2; /* Modified EUI-64, stable across reconnects. */
    s->iid[3] = 0xff;
    s->iid[4] = 0xfe;
    memcpy(s->iid + 5, s->mac + 3, 3);
    if (getrandom(s->token, sizeof(s->token), 0) != sizeof(s->token) ||
        getrandom(s->magic, sizeof(s->magic), 0) != sizeof(s->magic))
        return -EIO;
    s->disc = packet_socket(s->link, DISC);
    if (s->disc < 0)
        return -errno;
    /* Bind before PADR so the first LCP packet can already be queued. */
    s->data = packet_socket(s->link, SESS);
    if (s->data < 0)
        return -errno;
    int rc = discover(s, cfg->service);
    if (!rc)
        rc = negotiate(s, cfg, LCP);
    if (!rc)
        rc = authenticate(s, cfg);
    if (!rc)
        rc = negotiate(s, cfg, IPCP);
    if (!rc && cfg->ipv6) {
        if (s->mtu < 1280 || s->mru < 1280)
            return -EMSGSIZE;
        rc = negotiate(s, cfg, IPV6CP);
    }
    if (!rc)
        rc = open_tun(s, cfg, ctl);
    if (rc < 0)
        return rc;
    if ((rc = publish_dns(s, cfg->dns_file)) < 0)
        return rc;
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, s->address, ip, sizeof(ip)))
        return -errno;
    printf("PPPOE UP session=%u auth=%s ip=%s mtu=%u\n", s->id,
           s->auth == PAP ? "pap" : "chap", ip, s->mtu);
    if (cfg->ipv6) {
        uint8_t local[16] = {0xfe, 0x80}, remote[16] = {0xfe, 0x80};
        char a[INET6_ADDRSTRLEN], b[INET6_ADDRSTRLEN];
        memcpy(local + 8, s->iid, 8);
        memcpy(remote + 8, s->remote_iid, 8);
        if (inet_ntop(AF_INET6, local, a, sizeof(a)) &&
            inet_ntop(AF_INET6, remote, b, sizeof(b)))
            printf("PPPOE IPV6 local=%s peer=%s\n", a, b);
    }
    fflush(stdout);
    return online(s, cfg);
}

static bool read_credentials(const char *path, struct config *cfg)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077)) {
        close(fd);
        errno = EACCES;
        return false;
    }
    char data[514];
    size_t size = 0;
    while (size < sizeof(data)) {
        ssize_t n = read(fd, data + size, sizeof(data) - size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        size += (size_t)n;
    }
    close(fd);
    const char *end = data + size;
    const char *first = memchr(data, '\n', size);
    bool ok = false;
    if (first && !memchr(data, 0, size) && size < sizeof(data)) {
        const char *second = memchr(first + 1, '\n', (size_t)(end - first - 1));
        size_t user = (size_t)(first - data);
        size_t pass = (size_t)((second ? second : end) - first - 1);
        if (user && user < sizeof(cfg->user) && pass < sizeof(cfg->password) &&
            (!second || second + 1 == end)) {
            memcpy(cfg->user, data, user);
            memcpy(cfg->password, first + 1, pass);
            ok = true;
        }
    }
    erase(data, sizeof(data));
    if (!ok)
        errno = EINVAL;
    return ok;
}

int main(int argc, char **argv)
{
    struct config cfg = {0};
    int option;
    while ((option = getopt(argc, argv, "+6d:")) != -1) {
        if (option == '6')
            cfg.ipv6 = true;
        else if (option == 'd' && optarg[0])
            cfg.dns_file = optarg;
        else
            return 2;
    }
    argv += optind - 1;
    argc -= optind - 1;
    if (argc != 6 || !argv[1][0] || strlen(argv[1]) >= IFNAMSIZ ||
        !argv[2][0] || strlen(argv[2]) >= IFNAMSIZ || strlen(argv[3]) > 255 ||
        (strcmp(argv[4], "pap") && strcmp(argv[4], "chap"))) {
        fprintf(stderr, "usage: pppoe [-6] [-d DNS-FILE] WAN TUN SERVICE|- pap|chap CREDENTIALS\n");
        return 2;
    }
    cfg.wan = argv[1];
    cfg.tun = argv[2];
    cfg.service = strcmp(argv[3], "-") ? argv[3] : "";
    cfg.auth = !strcmp(argv[4], "pap") ? PAP : CHAP;
    if (cfg.dns_file) {
        struct stat status;
        if (lstat(cfg.dns_file, &status) == 0) {
            fprintf(stderr, "pppoe DNS output already exists: %s\n", cfg.dns_file);
            return 1;
        }
        if (errno != ENOENT) {
            perror("pppoe DNS output");
            return 1;
        }
    }
    if (!read_credentials(argv[5], &cfg)) {
        perror("pppoe credentials");
        return 1;
    }
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    int ctl = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ctl < 0) {
        perror("pppoe control socket");
        erase(cfg.password, sizeof(cfg.password));
        return 1;
    }
    unsigned backoff = 1;
    while (!stopping) {
        struct session s = {
            .disc = -1, .data = -1, .tun = -1, .mtu = MTU, .mru = MTU,
            .link = {.sll_family = AF_PACKET, .sll_halen = 6},
        };
        memcpy(s.peer, broadcast, 6);
        int rc = dial(&s, &cfg, ctl);
        remove_dns(&s, cfg.dns_file);
        if (s.tun >= 0)
            close(s.tun);
        if (s.id)
            (void)frame_send(&s, DISC, s.peer, PADT, s.id, s.token, 0);
        if (s.data >= 0)
            close(s.data);
        if (s.disc >= 0)
            close(s.disc);
        if (stopping)
            break;
        if (s.tun >= 0)
            backoff = 1;
        fprintf(stderr, "PPPOE DOWN %s, retry in %u s\n", strerror(-rc), backoff);
        sleep(backoff);
        if (backoff < 16)
            backoff *= 2;
    }
    close(ctl);
    erase(cfg.password, sizeof(cfg.password));
    return 0;
}
