/*
 * rust_idl_peer_wire_check.c — host/generator side check of the exact v1
 * svcman/echod wire bytes.
 *
 * The Rust peer (user/svc/rust_idl_peer.rs) carries the request bytes as hex
 * literals, because no host compiler can build its #![no_std] syscall layer.
 * That is exactly the kind of duplication that rots, so this program closes
 * the loop from the other end:
 *
 *   1. it is compiled against the header FRESHLY GENERATED from
 *      user/svc/a20_services.idl by tools/a20idl.py — not against a hand
 *      written copy and not against user/svc/a20_services_idl.h in the tree,
 *      so `make check-a20-idl`'s regeneration path is the one under test;
 *   2. it composes each request the way user/svc/svcman.c does, with the real
 *      generated typedefs, and serialises it twice — once by copying the
 *      struct (which exercises padding and natural alignment) and once by
 *      packing each field explicitly little-endian (which exercises the
 *      intended wire semantics) — and requires the two to be equal;
 *   3. it parses the `wire:<name>:<hex>` markers and the hex_bytes::<N>("...")
 *      literals back out of the Rust source and requires every one of them to
 *      match the bytes composed in step 2, and every hex_bytes() literal to
 *      appear as a wire marker (the drift check that keeps the two in step);
 *   4. it re-asserts the IDL's own version constant and the envelope layout
 *      the service validates on.
 *
 * Nothing here runs on QEMU and no peer is simulated: this is a byte-level
 * generator check.  The behavioural half — that the live C echod really echoes,
 * drops and crashes as those bytes predict — belongs to the guest-side peer
 * and its smoke gate.
 *
 * Build and run (from the repository root):
 *
 *   mkdir -p <tmp>
 *   python3 tools/a20idl.py user/svc/a20_services.idl <tmp>/a20_services_idl.h
 *   cc -std=gnu99 -Wall -Wextra -I<tmp> -Iuser/liba20rt \
 *      -o <tmp>/rust_idl_peer_wire_check \
 *      tools/rust_idl_peer_wire_check.c
 *   <tmp>/rust_idl_peer_wire_check user/svc/rust_idl_peer.rs
 *
 * Exit 0 = every byte agrees.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "a20_services_idl.h"

#define MAX_VECTORS 32
#define NAME_MAX_LEN 64
#define HEX_MAX_LEN 160

struct wire {
    char name[NAME_MAX_LEN];
    char hex[HEX_MAX_LEN];
};

static struct wire g_wire[MAX_VECTORS];
static size_t g_wire_count;
static int g_failures;

static void hex_of(const uint8_t *buf, size_t len, char *out)
{
    static const char d[] = "0123456789abcdef";
    size_t i;

    if (len * 2 + 1 > HEX_MAX_LEN) {
        strcpy(out, "<too long>");
        return;
    }
    for (i = 0; i < len; i++) {
        out[2 * i] = d[(buf[i] >> 4) & 0x0f];
        out[2 * i + 1] = d[buf[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

static int hex_equal(const char *hex, const uint8_t *buf, size_t len)
{
    char made[HEX_MAX_LEN];

    hex_of(buf, len, made);
    return strcmp(hex, made) == 0;
}

/* ---- explicit little-endian serialisation of the IDL fields ------------- */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* ---- Rust source parsing ---------------------------------------------- */

/*
 * Pull `wire:<name>:<hex>` out of line.  Returns 1 on a well-formed marker.
 * The markers live in the dock comment block above the hex_bytes() constants
 * in user/svc/rust_idl_peer.rs.
 */
static int parse_wire_marker(const char *line, char *name, char *hex)
{
    const char *p = strstr(line, "wire:");
    const char *colon;
    size_t n;

    if (p == NULL) {
        return 0;
    }
    p += 5;
    colon = strchr(p, ':');
    if (colon == NULL) {
        return 0;
    }
    n = (size_t)(colon - p);
    if (n == 0 || n >= NAME_MAX_LEN) {
        return 0;
    }
    memcpy(name, p, n);
    name[n] = '\0';
    snprintf(hex, HEX_MAX_LEN, "%s", colon + 1);
    /* trim to the first non-hex character (the line ends with \n) */
    for (n = 0; hex[n] != '\0'; n++) {
        char c = hex[n];
        int is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
        if (!is_hex) {
            break;
        }
    }
    hex[n] = '\0';
    return n > 0 && (n % 2) == 0;
}

static void record(const char *name, const char *hex)
{
    size_t i;

    for (i = 0; i < g_wire_count; i++) {
        if (strcmp(g_wire[i].name, name) == 0) {
            if (strcmp(g_wire[i].hex, hex) != 0) {
                printf("  FAIL rust: wire:%s:%s redefined as %s\n",
                       g_wire[i].name, g_wire[i].hex, hex);
                g_failures++;
            }
            return;
        }
    }
    if (g_wire_count == MAX_VECTORS) {
        printf("  FAIL rust: too many wire vectors\n");
        g_failures++;
        return;
    }
    snprintf(g_wire[g_wire_count].name, NAME_MAX_LEN, "%s", name);
    snprintf(g_wire[g_wire_count].hex, HEX_MAX_LEN, "%s", hex);
    g_wire_count++;
}

/*
 * Load the wire markers, and separately every hex_bytes::<N>("...") literal,
 * so both halves of the Rust table are read from the file actually under
 * review rather than from a copy of it here.
 */
static void load_rust_source(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];

    if (f == NULL) {
        printf("  FAIL rust: cannot open %s\n", path);
        g_failures++;
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char name[NAME_MAX_LEN];
        char hex[HEX_MAX_LEN];
        const char *lit = strstr(line, "hex_bytes::<");

        if (parse_wire_marker(line, name, hex)) {
            record(name, hex);
        }
        if (lit != NULL) {
            const char *q = strchr(lit + 12, '"');
            const char *end;
            int declared;

            if (q == NULL) {
                continue;
            }
            q++;
            end = strchr(q, '"');
            if (end == NULL || (size_t)(end - q) >= HEX_MAX_LEN) {
                continue;
            }
            memcpy(hex, q, (size_t)(end - q));
            hex[end - q] = '\0';
            /*
             * A hex literal is only legal as a wire marker: this is the drift
             * check that fails if someone changes a vector in one place.
             */
            declared = 0;
            for (size_t i = 0; i < g_wire_count; i++) {
                if (strcmp(g_wire[i].hex, hex) == 0) {
                    declared = 1;
                    break;
                }
            }
            if (!declared) {
                printf("  FAIL rust: hex_bytes literal \"%s\" has no "
                       "wire:<name>: marker\n", hex);
                g_failures++;
            }
        }
    }
    fclose(f);
}

static const char *wire_hex(const char *name)
{
    size_t i;

    for (i = 0; i < g_wire_count; i++) {
        if (strcmp(g_wire[i].name, name) == 0) {
            return g_wire[i].hex;
        }
    }
    printf("  FAIL rust: wire vector %s is not declared\n", name);
    g_failures++;
    return "";
}

/* ---- the six vectors, composed from the generated typedefs -------------- */

/*
 * Echo request, exactly user/svc/svcman.c:echo_verify():
 *
 *     struct { a20_idl_envelope_t env; a20_idl_svcmgr_echo_t body; } req;
 *     req.env = { A20_SERVICES_IDL_VERSION, SVCMGR_REQ_ECHO, sizeof(req) };
 */
static size_t compose_echo_exact(uint8_t *out)
{
    struct {
        a20_idl_envelope_t env;
        a20_idl_svcmgr_echo_t body;
    } req;

    memset(&req, 0, sizeof(req));
    req.env.version = A20_SERVICES_IDL_VERSION;
    req.env.type = SVCMGR_REQ_ECHO;
    req.env.size = (uint32_t)sizeof(req);
    req.body.seq = 5u;

    if (sizeof(req) != 12) {
        printf("  FAIL idl: echo request is %zu bytes, expected 12\n",
               sizeof(req));
        g_failures++;
    }
    /* The same bytes, packed by hand. */
    put_u16(out, req.env.version);
    put_u16(out + 2, req.env.type);
    put_u32(out + 4, req.env.size);
    put_u32(out + 8, req.body.seq);
    /* ...and the struct copy, which must agree. */
    memcpy(out + 16, &req, sizeof(req));
    return (size_t)sizeof(req);
}

/*
 * Same envelope, a wire type the IDL does not define.  echod has no case for
 * it, falls through to the echo, and must return it verbatim.
 */
static size_t compose_echo_unknown_type(uint8_t *out)
{
    a20_idl_envelope_t env;
    uint8_t scratch[16];

    env.version = A20_SERVICES_IDL_VERSION;
    env.type = 0x7bu; /* not in a20_services.idl */
    env.size = 12;

    put_u16(out, env.version);
    put_u16(out + 2, env.type);
    put_u32(out + 4, env.size);
    put_u32(out + 8, 0xdeadbeefu);
    memcpy(scratch, &env, sizeof(env));
    if (memcmp(scratch, out, sizeof(env)) != 0) {
        printf("  FAIL idl: envelope struct copy != explicit LE packing\n");
        g_failures++;
    }
    return 12;
}

/*
 * The crash request user/svc/svcman.c sends: the bare envelope, no body.
 */
static size_t compose_crash_request(uint8_t *out)
{
    a20_idl_envelope_t env;

    env.version = A20_SERVICES_IDL_VERSION;
    env.type = SVCMGR_REQ_CRASH;
    env.size = (uint32_t)sizeof(env);

    put_u16(out, env.version);
    put_u16(out + 2, env.type);
    put_u32(out + 4, env.size);
    if (memcmp(&env, out, sizeof(env)) != 0) {
        printf("  FAIL idl: crash envelope struct copy != explicit LE "
               "packing\n");
        g_failures++;
    }
    return (size_t)sizeof(env);
}

/* version 2: a version the receiver rejects outright. */
static size_t compose_drop_version_mismatch(uint8_t *out)
{
    put_u16(out, 2u);
    put_u16(out + 2, SVCMGR_REQ_ECHO);
    put_u32(out + 4, 12u);
    put_u32(out + 8, 0xaabbccddu);
    return 12;
}

/* version 1, but `size` lies about a 12-byte message. */
static size_t compose_drop_size_mismatch(uint8_t *out)
{
    put_u16(out, A20_SERVICES_IDL_VERSION);
    put_u16(out + 2, SVCMGR_REQ_ECHO);
    put_u32(out + 4, 9u);
    put_u32(out + 8, 0xaabbccddu);
    return 12;
}

/* Four bytes: shorter than sizeof(a20_idl_envelope_t). */
static size_t compose_drop_short_message(uint8_t *out)
{
    put_u16(out, A20_SERVICES_IDL_VERSION);
    put_u16(out + 2, SVCMGR_REQ_ECHO);
    return 4;
}

/* ---- checks ------------------------------------------------------------ */

static void check_idl_layout(void)
{
    if (A20_SERVICES_IDL_VERSION != 1u) {
        printf("  FAIL idl: generated version is %u, expected 1\n",
               A20_SERVICES_IDL_VERSION);
        g_failures++;
    }
    if (A20_SVC_CRASH_CODE != 42) {
        printf("  FAIL idl: A20_SVC_CRASH_CODE is %d\n", A20_SVC_CRASH_CODE);
        g_failures++;
    }
    if (SVCMGR_REQ_ECHO != 0x21u || SVCMGR_REQ_CRASH != 0x22u) {
        printf("  FAIL idl: SVCMGR request constants moved\n");
        g_failures++;
    }
    if (sizeof(a20_idl_envelope_t) != 8) {
        printf("  FAIL idl: a20_idl_envelope_t is %zu bytes, expected 8\n",
               sizeof(a20_idl_envelope_t));
        g_failures++;
    }
    if (offsetof(a20_idl_envelope_t, version) != 0 ||
        offsetof(a20_idl_envelope_t, type) != 2 ||
        offsetof(a20_idl_envelope_t, size) != 4) {
        printf("  FAIL idl: a20_idl_envelope_t field offsets moved\n");
        g_failures++;
    }
    if (sizeof(a20_idl_svcmgr_echo_t) != 4 ||
        offsetof(a20_idl_svcmgr_echo_t, seq) != 0) {
        printf("  FAIL idl: a20_idl_svcmgr_echo_t layout changed\n");
        g_failures++;
    }
}

static void check_vector(const char *name, const uint8_t *bytes, size_t len)
{
    const char *want = wire_hex(name);
    char made[HEX_MAX_LEN];

    hex_of(bytes, len, made);
    printf("  %-22s %s (%zu bytes)\n", name, made, len);
    if (!hex_equal(want, bytes, len)) {
        printf("  FAIL %s: rust says %s, generated header says %s\n",
               name, want, made);
        g_failures++;
    }
}

int main(int argc, char **argv)
{
    uint8_t buf[32];
    uint8_t echo[32];
    size_t n;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <path to user/svc/rust_idl_peer.rs>\n",
                argv[0]);
        return 2;
    }

    printf("idl wire check: header generated from user/svc/a20_services.idl "
           "by tools/a20idl.py\n");
    check_idl_layout();

    load_rust_source(argv[1]);
    if (g_wire_count != 6) {
        printf("  FAIL rust: expected 6 wire vectors, found %zu\n",
               g_wire_count);
        g_failures++;
    }

    /* The struct copy of the echo request must sit at buf+16 and agree. */
    n = compose_echo_exact(buf);
    memcpy(echo, buf, n);
    if (memcmp(echo, buf + 16, n) != 0) {
        printf("  FAIL idl: echo struct copy and explicit LE packing "
               "disagree\n");
        g_failures++;
    }
    check_vector("echo_exact", echo, n);

    n = compose_echo_unknown_type(buf);
    check_vector("echo_unknown_type", buf, n);

    n = compose_crash_request(buf);
    check_vector("crash_request", buf, n);

    n = compose_drop_version_mismatch(buf);
    check_vector("drop_version_mismatch", buf, n);

    n = compose_drop_size_mismatch(buf);
    check_vector("drop_size_mismatch", buf, n);

    n = compose_drop_short_message(buf);
    check_vector("drop_short_message", buf, n);

    if (g_failures != 0) {
        printf("IDL_WIRE: FAIL (%d mismatch(es))\n", g_failures);
        return 1;
    }
    printf("IDL_WIRE: PASS\n");
    return 0;
}
