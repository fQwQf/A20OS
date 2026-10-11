/* Allocation-free deployment configuration and bounded restart policy.
 * Parsing has no side effects and publishes only a fully validated snapshot. */
#ifndef A20_CLUSTER_CONFIG_H
#define A20_CLUSTER_CONFIG_H
#include <stdint.h>
#ifndef A20_CONFIG_SEEDS
#define A20_CONFIG_SEEDS 16u
#endif
#define A20_CONFIG_BYTES 4096u
#define A20_CONFIG_PATH 128u
#define A20_CONFIG_SERVICES 3u
#define A20_CONFIG_AGENT 0u
#define A20_CONFIG_CONTROL 1u
#define A20_CONFIG_JOBS 2u

typedef struct a20_config_seed {
    uint8_t node[16], hop[6];
    uint32_t transport, hop_len;
} a20_config_seed_t;
typedef struct a20_cluster_config {
    uint8_t node[16];
    uint32_t caps, seed_count;
    a20_config_seed_t seeds[A20_CONFIG_SEEDS];
    char service[A20_CONFIG_SERVICES][A20_CONFIG_PATH];
    char process[A20_CONFIG_PATH];
} a20_cluster_config_t;
static inline int a20_config_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
static inline int a20_config_id(const char *s, uint8_t out[16]) {
    unsigned any = 0, all = 255;
    for (unsigned i = 0; i < 16; i++) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; j++) {
            unsigned c = (unsigned char)*s++, d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return -1;
            value = value * 16 + d;
        }
        out[i] = value;
        any |= value;
        all &= value;
    }
    return *s || !any || all == 255 ? -1 : 0;
}
static inline int a20_config_same_id(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 16; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static inline int a20_config_path(char out[A20_CONFIG_PATH], const char *s) {
    if (*s != '/')
        return -1;
    unsigned i = 0;
    while (s[i]) {
        if (i >= A20_CONFIG_PATH - 1 || (unsigned char)s[i] < 33 ||
            (unsigned char)s[i] > 126)
            return -1;
        out[i] = s[i];
        i++;
    }
    out[i] = 0;
    return i > 1 ? 0 : -1;
}
static inline int a20_config_address(const char *s, a20_config_seed_t *seed) {
    if (a20_config_equal(s, "uart:1")) {
        seed->transport = 2;
        seed->hop_len = 2;
        seed->hop[0] = 1;
        return 0;
    }
    const char *prefix = "udp:";
    for (unsigned i = 0; i < 4; i++)
        if (*s++ != prefix[i])
            return -1;
    for (unsigned i = 0; i < 5; i++) {
        unsigned n = 0, digits = 0, max = i == 4 ? 65535u : 255u;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (unsigned)(*s++ - '0');
            if (++digits > 5 || n > max)
                return -1;
        }
        if (!digits)
            return -1;
        if (i < 4) {
            seed->hop[i] = n;
            if (*s++ != (i == 3 ? ':' : '.'))
                return -1;
        } else {
            if (!n || *s)
                return -1;
            seed->hop[4] = n >> 8;
            seed->hop[5] = n;
        }
    }
    if (!seed->hop[0] || seed->hop[0] >= 224)
        return -1;
    seed->transport = 1;
    seed->hop_len = 6;
    return 0;
}
static inline int a20_cluster_config_parse(const char *text, uint32_t len,
                                           a20_cluster_config_t *out,
                                           uint32_t *error_line) {
    a20_cluster_config_t c = {0};
    unsigned seen = 0, line_no = 0, pos = 0, features = 0;
    if (error_line)
        *error_line = 0;
    if (!text || !out || !len || len > A20_CONFIG_BYTES)
        return -1;
    while (pos < len) {
        char line[192];
        unsigned n = 0;
        line_no++;
        while (pos < len && text[pos] != '\n') {
            unsigned ch = (unsigned char)text[pos++];
            if (!ch || (ch < 32 && ch != '\t' && ch != '\r') || ch > 126 ||
                n == sizeof(line) - 1)
                goto bad;
            line[n++] = ch;
        }
        if (pos < len)
            pos++;
        line[n] = 0;
        char *tokens[4] = {0};
        unsigned count = 0, i = 0;
        while (i < n) {
            while (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')
                i++;
            if (!line[i] || line[i] == '#')
                break;
            if (count == 4)
                goto bad;
            tokens[count++] = line + i;
            while (line[i] && line[i] != ' ' && line[i] != '\t' &&
                   line[i] != '\r')
                i++;
            if (line[i])
                line[i++] = 0;
        }
        if (!count)
            continue;
        if (a20_config_equal(tokens[0], "version") && count == 2) {
            if ((seen & 1) || !a20_config_equal(tokens[1], "1"))
                goto bad;
            seen |= 1;
        } else if (a20_config_equal(tokens[0], "node") && count == 2) {
            if ((seen & 2) || a20_config_id(tokens[1], c.node))
                goto bad;
            seen |= 2;
        } else if (a20_config_equal(tokens[0], "role") && count == 2) {
            if (seen & 4)
                goto bad;
            if (a20_config_equal(tokens[1], "worker"))
                c.caps = 2;
            else if (a20_config_equal(tokens[1], "relay"))
                c.caps = 3;
            else if (a20_config_equal(tokens[1], "leaf"))
                c.caps = 6;
            else
                goto bad;
            seen |= 4;
        } else if (a20_config_equal(tokens[0], "feature") && count == 2) {
            if (features || !a20_config_equal(tokens[1], "oneway"))
                goto bad;
            features = 16;
        } else if (a20_config_equal(tokens[0], "seed") && count == 3) {
            if (c.seed_count == A20_CONFIG_SEEDS)
                goto bad;
            a20_config_seed_t *s = &c.seeds[c.seed_count];
            if (a20_config_id(tokens[1], s->node) ||
                a20_config_address(tokens[2], s))
                goto bad;
            for (unsigned j = 0; j < c.seed_count; j++)
                if (a20_config_same_id(s->node, c.seeds[j].node))
                    goto bad;
            c.seed_count++;
        } else if (a20_config_equal(tokens[0], "service") && count == 3) {
            unsigned service = A20_CONFIG_SERVICES;
            if (a20_config_equal(tokens[1], "agent"))
                service = A20_CONFIG_AGENT;
            if (a20_config_equal(tokens[1], "control"))
                service = A20_CONFIG_CONTROL;
            if (a20_config_equal(tokens[1], "jobs"))
                service = A20_CONFIG_JOBS;
            if (service == A20_CONFIG_SERVICES || c.service[service][0] ||
                a20_config_path(c.service[service], tokens[2]))
                goto bad;
        } else if (a20_config_equal(tokens[0], "process") && count == 2) {
            if (c.process[0] || a20_config_path(c.process, tokens[1]))
                goto bad;
        } else
            goto bad;
    }
    if (seen != 7 || (c.process[0] && !c.service[A20_CONFIG_AGENT][0]))
        goto bad;
    c.caps |= features;
    for (unsigned i = 0; i < c.seed_count; i++)
        if (a20_config_same_id(c.node, c.seeds[i].node))
            goto bad;
    *out = c;
    return 0;
bad:
    if (error_line)
        *error_line = line_no;
    return -1;
}

typedef struct a20_restart_policy {
    uint64_t starts, exits, next_start, started;
    uint32_t failures, exhausted;
} a20_restart_policy_t;
static inline void a20_restart_failed(a20_restart_policy_t *p, uint64_t now) {
    if (p->exits == UINT64_MAX || p->starts == UINT64_MAX) {
        p->exhausted = 1;
        return;
    }
    p->exits++;
    if (p->started && now - p->started >= 60000)
        p->failures = 0;
    if (p->failures < 7)
        p->failures++;
    uint64_t delay = 100u << (p->failures - 1);
    p->next_start = now + delay;
    if (p->next_start < now)
        p->exhausted = 1;
}
#endif
