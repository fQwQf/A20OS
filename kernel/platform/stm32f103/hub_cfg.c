/*
 * Hub config parser. Pure text logic — see hub_cfg.h. Covered by the host
 * unit test.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "hub_cfg.h"

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive compare of a [s,e) span with a lowercase literal. */
static int span_ieq(const char *s, const char *e, const char *lit) {
    while (s < e && *lit) {
        if (lower(*s) != *lit)
            return 0;
        s++;
        lit++;
    }
    return s == e && *lit == '\0';
}

/* Copy a [s,e) span into a fixed buffer, stripping one layer of surrounding
 * double quotes if present. */
static void span_copy(const char *s, const char *e, char *out, unsigned cap) {
    if (e > s && s[0] == '"' && e[-1] == '"') {
        s++;
        e--;
    }
    unsigned n = 0;
    while (s < e && n + 1U < cap)
        out[n++] = *s++;
    out[n] = '\0';
}

static unsigned span_to_uint(const char *s, const char *e) {
    unsigned v = 0;
    while (s < e && *s >= '0' && *s <= '9') {
        v = v * 10U + (unsigned)(*s - '0');
        s++;
    }
    return v;
}

int hub_cfg_parse(const char *text, unsigned len, hub_cfg_t *out) {
    if (!text || !out)
        return -1;
    for (unsigned i = 0; i < sizeof(*out); i++)
        ((char *)out)[i] = 0;

    int recognised = 0;
    unsigned i = 0;
    while (i < len) {
        /* isolate one line [ls, le) */
        unsigned ls = i;
        while (i < len && text[i] != '\n')
            i++;
        unsigned le = i;
        if (i < len)
            i++; /* skip '\n' */

        const char *p = text + ls;
        const char *end = text + le;
        while (p < end && is_space(*p)) /* ltrim */
            p++;
        while (end > p && is_space(end[-1])) /* rtrim */
            end--;
        if (p == end || *p == '#')
            continue;

        /* split key=value */
        const char *eq = p;
        while (eq < end && *eq != '=')
            eq++;
        if (eq == end)
            continue; /* no '=' */
        const char *ks = p, *ke = eq;
        const char *vs = eq + 1, *ve = end;
        while (ke > ks && is_space(ke[-1])) /* trim key */
            ke--;
        while (vs < ve && is_space(*vs)) /* trim value */
            vs++;

        if (span_ieq(ks, ke, "ssid")) {
            span_copy(vs, ve, out->ssid, sizeof(out->ssid));
            out->have_ssid = out->ssid[0] ? 1 : 0;
            recognised += out->have_ssid;
        } else if (span_ieq(ks, ke, "pass") || span_ieq(ks, ke, "password")) {
            span_copy(vs, ve, out->pass, sizeof(out->pass));
            out->have_pass = 1; /* an empty password is legitimate (open AP) */
            recognised++;
        } else if (span_ieq(ks, ke, "proxy")) {
            const char *colon = vs;
            while (colon < ve && *colon != ':')
                colon++;
            if (colon < ve) {
                span_copy(vs, colon, out->proxy_ip, sizeof(out->proxy_ip));
                unsigned port = span_to_uint(colon + 1, ve);
                if (out->proxy_ip[0] && port > 0 && port <= 65535) {
                    out->proxy_port = (uint16_t)port;
                    out->have_proxy = 1;
                    recognised++;
                }
            }
        }
    }
    return recognised;
}

#endif /* CONFIG_BOARD_STM32F103 */
