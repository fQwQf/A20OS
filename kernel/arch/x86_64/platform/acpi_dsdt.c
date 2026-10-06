#ifdef CONFIG_X86_64

/*
 * Enough AML to answer one question: where does a PCI device's INTx line go?
 *
 * The answer lives in the DSDT, in a _PRT package under the scope describing the
 * bus the device sits on.  Reading it means walking the namespace and evaluating
 * the package, which is the slice of AML this file implements.  It is a
 * deliberately small subset, and everything it does not implement makes the
 * lookup report "no routing" rather than guess:
 *
 * - Constants, packages, and the integer forms an address is written in.  No
 *   strings, no regions, no fields, no control flow, and no method calls: a
 *   _PRT that refers to another object fails instead of evaluating that object
 *   as zero.
 *
 * The failure mode is the design, not a side effect.  A wrong GSI is worse than
 * no GSI: firmware has usually already programmed that IOAPIC entry, so a wrong
 * answer does not merely fail to help, it takes a line away from whichever
 * device firmware meant to own it.  No answer leaves the caller on its polling
 * fallback, which is slower and safe.
 *
 * This is not a general ACPI implementation and does not claim to be one.  It
 * reads the shape QEMU generates and the shapes real firmware is most likely to
 * emit; it has never been run against a DSDT from real hardware.  See
 * docs/platforms/x86_64-pc.md for what that leaves unverified.
 */

#include "core/types.h"
#include "firmware.h"
#include "platform.h"
#include "core/string.h"
#include "core/klog.h"

/* Opcodes, named only where the encoding has to be measured or evaluated. */
#define AML_ZERO        0x00
#define AML_ONE         0x01
#define AML_ONES        0x02
#define AML_BYTECONST   0x0C
#define AML_LEADINGBYTE 0x0D
#define AML_WORDCONST   0x0E
#define AML_DWORDCONST  0x0F
#define AML_SCOPE       0x10   /* Scope, DefScope, ExtendedFieldPrefix */
#define AML_BUFFER      0x11
#define AML_PACKAGE     0x12
#define AML_METHOD      0x14   /* DefMethod, Method */
#define AML_DEFPACKAGE  0x15
#define AML_EXTERNAL    0x16
#define AML_NAMEOP      0x17
#define AML_ALIAS       0x19
#define AML_DEFBANK     0x1A
#define AML_DEVICE      0x1C
#define AML_EVENT       0x1D
#define AML_DEFFUNCTION 0x1E
#define AML_DEFMUTEX    0x23
#define AML_DEFPOWER    0x30
#define AML_DEFTHERMAL  0x31
#define AML_IDXFIELD    0x32
#define AML_BANKFIELD   0x33
#define AML_RETURN      0x6B
#define AML_EXTPREFIX   0x5B

#define AML_MAX_SCOPES  128
#define AML_PATH_MAX    160
#define AML_MAX_PKG     64
#define AML_MAX_DEPTH   4

typedef struct {
    uint64_t value;
    int      is_package;
} aml_obj_t;

typedef struct {
    char           path[AML_PATH_MAX];
    uint32_t       adr;
    int            has_adr;
    const uint8_t *prt_obj;    /* start of the term that builds the package */
    uint32_t       prt_len;
} aml_scope_t;

static aml_scope_t g_scope[AML_MAX_SCOPES];
static unsigned g_scope_count;

/* Depth-0 package built by the most recent evaluation, and its children. */
static aml_obj_t g_pkg[AML_MAX_DEPTH][AML_MAX_PKG];
static unsigned g_pkg_count[AML_MAX_DEPTH];

static int aml_eval(const uint8_t **pp, const uint8_t *end, aml_obj_t *obj);

/* PkgLength: a lead byte naming how many length bytes follow it, then the
 * length.  The length covers the encoding as well as the data, which is what a
 * caller advancing past the term wants. */
static int aml_pkg_length(const uint8_t **pp, const uint8_t *end, uint32_t *length)
{
    if (*pp >= end)
        return -1;
    uint8_t lead = **pp;
    uint32_t width;
    switch (lead >> 6) {
    case 0: width = 1; break;
    case 1: width = 2; break;
    case 2: width = 3; break;
    default: width = 4; break;
    }
    if ((size_t)(end - *pp) < 1 + width)
        return -1;
    uint32_t value = lead & 0x3FU;
    for (uint32_t i = 1; i < width; i++)
        value |= (uint32_t)(*pp)[i] << (i * 8);
    *pp += 1 + width;
    *length = value;
    return 0;
}

/* One segment of a NameString.
 *
 * The lead byte's top two bits select the form (ACPI 6.4 section 5.3):
 *
 *   00  the low six bits are the character count, 1 to 4
 *   40  eight characters follow
 *   80  four follow
 *   C0  two follow
 *
 * The compressed forms exist so hardware IDs can be written without paying for
 * a lead byte per character.  None of them name an object this file looks for,
 * but their bytes still have to be consumed the right number, or everything
 * after them in the name is read as part of the name. */
static int aml_seg(const uint8_t **pp, const uint8_t *end, char *out,
                   size_t *len)
{
    const uint8_t *p = *pp;
    if (p >= end)
        return -1;

    uint8_t lead = *p++;
    size_t n;
    switch (lead & 0xC0U) {
    case 0x00: n = lead & 0x3FU; break;
    case 0x40: n = 8; break;
    case 0x80: n = 4; break;
    default:   n = 2; break;
    }
    if (n == 0 || n > 8 || (size_t)(end - p) < n)
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (*len + 1 < AML_PATH_MAX)
            out[(*len)++] = (char)p[i];
    }
    *pp = p + n;
    return 0;
}

/* NameString -> a full path.
 *
 * Three forms exist and all three have to resolve against the enclosing scope:
 * a leading '\' names from the root, '^' steps to the parent, and anything else
 * is relative to the scope being walked.  Producing the full path here means no
 * caller ever has to resolve a name itself. */
static int aml_name(const uint8_t **pp, const uint8_t *end, const char *prefix,
                    char *out, size_t out_size)
{
    const uint8_t *p = *pp;
    if (p >= end)
        return -1;

    char buf[AML_PATH_MAX];
    size_t len = 0;

    if (*p == '\\') {
        p++;
        if (aml_seg(&p, end, buf, &len) != 0)
            return -1;
    } else if (*p == '^') {
        /* Steps out of the enclosing scope.  Walking back to the previous '.'
         * leaves the root intact, which is what stepping out of \_SB_ means. */
        p++;
        size_t plen = strlen(prefix);
        if (plen == 0 || plen >= AML_PATH_MAX)
            return -1;
        len = plen;
        if (buf[len - 1] != '\\') {
            while (len && buf[len - 1] != '.')
                len--;
            if (len > 1)
                len--;
        } else {
            len = 0;  /* already at the root: stay there */
        }
        if (p < end && *p != '^' && *p != '\\') {
            if (len + 1 >= AML_PATH_MAX)
                return -1;
            buf[len++] = '.';
            if (aml_seg(&p, end, buf, &len) != 0)
                return -1;
        }
    } else {
        /* Relative to the scope being walked.  Producing the full path here
         * means no caller ever has to resolve a name itself. */
        size_t plen = strlen(prefix);
        if (plen == 0 || plen >= AML_PATH_MAX)
            return -1;
        memcpy(buf, prefix, plen);
        len = plen;
        if (buf[len - 1] != '\\') {
            if (len + 1 >= AML_PATH_MAX)
                return -1;
            buf[len++] = '.';
        }
        if (aml_seg(&p, end, buf, &len) != 0)
            return -1;
    }

    /* Any number of further segments, each introduced by a '.'. */
    while (p < end && *p == '.') {
        p++;
        if (aml_seg(&p, end, buf, &len) != 0)
            return -1;
    }

    if (len == 0 || len + 1 > out_size)
        return -1;
    buf[len] = '\0';
    memcpy(out, buf, len + 1);
    *pp = p;
    return 0;
}

/* Build a package at @depth from the terms inside its own length. */
static int aml_eval_package(const uint8_t **pp, const uint8_t *end,
                            uint32_t body_len, int depth)
{
    if (depth >= AML_MAX_DEPTH)
        return -1;
    const uint8_t *body = *pp;
    const uint8_t *stop = body + body_len;
    if (stop > end)
        return -1;

    /* Element count follows the length.  More than 255 elements needs the
     * extended encoding, which this subset does not implement; a _PRT that
     * large is more than a root bus can describe anyway. */
    uint32_t count;
    if (body + 2 <= stop && *body == AML_BYTECONST) {
        count = body[1];
        body += 2;
    } else if (body + 1 <= stop && *body == AML_ONES) {
        count = 0xFFU;
        body += 1;
    } else {
        return -1;
    }

    g_pkg_count[depth] = 0;
    const uint8_t *p = body;
    while (p < stop && g_pkg_count[depth] < count) {
        aml_obj_t elem;
        if (aml_eval(&p, stop, &elem) != 0)
            return -1;
        if (g_pkg_count[depth] >= AML_MAX_PKG)
            return -1;
        g_pkg[depth][g_pkg_count[depth]++] = elem;
    }
    if (p != stop)
        return -1;    /* declared count and actual terms disagree */

    *pp = stop;
    return 0;
}

static int aml_eval(const uint8_t **pp, const uint8_t *end, aml_obj_t *obj) {
    const uint8_t *p = *pp;
    if (p >= end)
        return -1;

    obj->is_package = 0;
    obj->value = 0;

    switch (*p) {
    case AML_ZERO:
        p += 1; break;
    case AML_ONE:
        obj->value = 1;
        p += 1;
        break;
    case AML_ONES:
        if (p + 2 > end) return -1;
        obj->value = 0xFF00U | p[1];
        p += 2;
        break;
    case AML_BYTECONST:
        if (p + 2 > end) return -1;
        obj->value = p[1];
        p += 2;
        break;
    case AML_LEADINGBYTE:
        /* Carries a ByteConst or a DWordConst depending on the byte after it:
         * 0x80 and above is the value, below that it is the top byte of a
         * 32-bit one. */
        if (p + 2 > end) return -1;
        if (p[1] >= 0x80) {
            obj->value = p[1];
            p += 2;
        } else {
            if (p + 5 > end) return -1;
            obj->value = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                         ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
            p += 5;
        }
        break;
    case AML_WORDCONST:
        if (p + 3 > end) return -1;
        obj->value = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
        p += 3;
        break;
    case AML_DWORDCONST:
        if (p + 5 > end) return -1;
        obj->value = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                     ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
        p += 5;
        break;
    case AML_PACKAGE: {
        uint32_t len;
        p++;
        if (aml_pkg_length(&p, end, &len) != 0)
            return -1;
        if (aml_eval_package(&p, end, len, 0) != 0)
            return -1;
        obj->is_package = 1;
        break;
    }
    case AML_BUFFER: {
        uint32_t len;
        p++;
        if (aml_pkg_length(&p, end, &len) != 0)
            return -1;
        p += len;
        if (p > end)
            return -1;
        break;
    }
    default:
        return -1;
    }

    *pp = p;
    return 0;
}

/* Step over one term without evaluating it.  This is what makes a namespace walk
 * possible at all: a scope's children include devices, banks, methods and
 * arbitrary expressions, and none of them matter except that they have to be
 * stepped over exactly.  An opcode not listed stops the walk, which surfaces as
 * "no routing" rather than as a wrong GSI. */
static int aml_skip(const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp;
    if (p >= end)
        return -1;

    uint32_t len;
    char name[AML_PATH_MAX];

    switch (*p) {
    case AML_ZERO:
    case AML_ONE:
        p += 1;
        break;
    case AML_ONES:
    case AML_BYTECONST:
        p += 2;
        break;
    case AML_LEADINGBYTE:
        if (p + 2 > end) return -1;
        p += (p[1] >= 0x80) ? 2 : 5;
        break;
    case AML_WORDCONST:
        p += 3;
        break;
    case AML_DWORDCONST:
        p += 5;
        break;
    case AML_BUFFER: {
        p++;
        if (aml_pkg_length(&p, end, &len) != 0) return -1;
        p += len;
        break;
    }

    /* A scope: the length already covers the body, so its children are never
     * measured from here. */
    case AML_SCOPE: {
        p++;
        if (aml_pkg_length(&p, end, &len) != 0) return -1;
        p += len;
        break;
    }

    /* A name followed by an object that carries its own length. */
    case AML_NAMEOP:
    case AML_ALIAS: {
        p++;
        if (aml_name(&p, end, "\\", name, sizeof(name)) != 0) return -1;
        if (aml_skip(&p, end) != 0) return -1;
        break;
    }

    /* A name, a fixed header byte or two, then a body of known length. */
    case AML_METHOD:
    case AML_DEFPACKAGE:
    case AML_DEFBANK:
    case AML_DEVICE:
    case AML_DEFFUNCTION:
    case AML_DEFPOWER:
    case AML_DEFTHERMAL:
    case AML_IDXFIELD:
    case AML_BANKFIELD: {
        p++;
        if (aml_pkg_length(&p, end, &len) != 0) return -1;
        p += len;
        break;
    }
    case AML_DEFMUTEX: {
        /* DefMutex carries the sync level in the byte after the name, inside
         * the length. */
        p++;
        if (aml_pkg_length(&p, end, &len) != 0) return -1;
        p += len;
        break;
    }

    case AML_EVENT: {                /* name only, no length to hide behind */
        p++;
        if (aml_name(&p, end, "\\", name, sizeof(name)) != 0) return -1;
        break;
    }

    case AML_EXTERNAL: {
        p++;
        if (aml_name(&p, end, "\\", name, sizeof(name)) != 0) return -1;
        if (p + 4 > end) return -1;
        uint32_t datalen = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4 + datalen;
        break;
    }

    case AML_EXTPREFIX: {
        if (p + 2 > end) return -1;
        uint8_t op2 = p[1];
        p += 2;
        switch (op2) {
        case 0x00:                    /* External: name length, name, data */
            if (p + 2 > end) return -1;
            {
                uint32_t nlen = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
                p += 2 + nlen;
            }
            if (p + 4 > end) return -1;
            {
                uint32_t dlen = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                p += 4 + dlen;
            }
            break;
        case 0x01:                    /* Sleep: a 32-bit time */
            p += 4;
            break;
        default:
            /* Timer and field definitions only appear inside a method or
             * device, where the enclosing length already covers them. */
            return -1;
        }
        break;
    }

    default:
        return -1;
    }

    if (p > end)
        return -1;
    *pp = p;
    return 0;
}

static void aml_scope_add(const char *path)
{
    if (g_scope_count >= AML_MAX_SCOPES)
        return;
    aml_scope_t *s = &g_scope[g_scope_count++];
    size_t len = strlen(path);
    if (len >= AML_PATH_MAX)
        len = AML_PATH_MAX - 1;
    memcpy(s->path, path, len);
    s->path[len] = '\0';
    s->adr = 0;
    s->has_adr = 0;
    s->prt_obj = NULL;
    s->prt_len = 0;
}

/* Walk a TermList, recording every scope and, in the scope being entered, its
 * _ADR and _PRT. */
static int aml_walk(const uint8_t *p, const uint8_t *end, const char *prefix,
                    aml_scope_t *own)
{
    while (p < end) {
        if (*p == AML_SCOPE) {
            const uint8_t *q = p + 1;
            uint32_t len;
            if (aml_pkg_length(&q, end, &len) != 0)
                return -1;
            const uint8_t *body = q;
            const uint8_t *stop = body + len;
            if (stop > end)
                return -1;
            char path[AML_PATH_MAX];
            if (aml_name(&q, stop, prefix, path, sizeof(path)) != 0)
                return -1;

            unsigned before = g_scope_count;
            aml_scope_add(path);
            aml_scope_t *child = (g_scope_count > before)
                                 ? &g_scope[g_scope_count - 1] : NULL;
            if (aml_walk(body, stop, path, child) != 0)
                return -1;
            p = stop;
            continue;
        }

        if (*p == AML_NAMEOP) {
            const uint8_t *q = p + 1;
            char name[AML_PATH_MAX];
            if (aml_name(&q, end, prefix, name, sizeof(name)) != 0)
                return -1;
            const uint8_t *obj = q;
            /* _ADR is the only named value acted on: it is what says which bus
             * a scope describes. */
            if (own && !own->has_adr && strcmp(name, "_ADR") == 0) {
                aml_obj_t v;
                const uint8_t *r = q;
                if (aml_eval(&r, end, &v) != 0 || v.is_package)
                    return -1;
                own->adr = (uint32_t)v.value;
                own->has_adr = 1;
                q = r;
            } else if (own && !own->prt_obj && strcmp(name, "_PRT") == 0) {
                own->prt_obj = obj;
                own->prt_len = (uint32_t)(end - obj);
                if (aml_skip(&q, end) != 0)
                    return -1;
            } else if (aml_skip(&q, end) != 0) {
                return -1;
            }
            p = q;
            continue;
        }

        if (*p == AML_METHOD) {
            const uint8_t *q = p + 1;
            uint32_t len;
            if (aml_pkg_length(&q, end, &len) != 0)
                return -1;
            const uint8_t *body = q;
            const uint8_t *stop = body + len;
            if (stop > end)
                return -1;
            char name[AML_PATH_MAX];
            if (aml_name(&q, stop, prefix, name, sizeof(name)) != 0)
                return -1;
            if (q < stop)
                q++;                  /* method flags */
            if (own && !own->prt_obj && strcmp(name, "_PRT") == 0) {
                own->prt_obj = q;
                own->prt_len = (uint32_t)(stop - q);
            }
            p = stop;
            continue;
        }

        if (aml_skip(&p, end) != 0)
            return -1;
    }
    return 0;
}

static int dsdt_loaded;
static int dsdt_usable;

static void dsdt_load(void) {
    if (dsdt_loaded)
        return;
    dsdt_loaded = 1;

    uint32_t len = 0;
    const uint8_t *body = firmware_acpi_table("DSDT", &len);
    if (!body || len < 16)
        return;

    aml_scope_add("\\");
    if (aml_walk(body, body + len, "\\", &g_scope[0]) != 0) {
        /* A DSDT this subset cannot walk is a DSDT this subset does not answer
         * for.  Say so once: a driver quietly falling back to polling is
         * indistinguishable from a device that is simply idle. */
        unsigned scopes = g_scope_count;
        kinfo("[ACPI] DSDT %u bytes not walked (%u scopes before an opcode "
              "this subset does not implement); INTx stays on its polling "
              "fallback\n", (unsigned)len, scopes);
        return;
    }

    unsigned prts = 0;
    for (unsigned i = 0; i < g_scope_count; i++)
        prts += g_scope[i].prt_obj ? 1u : 0u;
    dsdt_usable = 1;
    kinfo("[ACPI] DSDT %u bytes: %u scopes, %u _PRT tables\n",
          (unsigned)len, g_scope_count, prts);
}

/* Evaluate the term a scope's _PRT is built from.  Both shapes firmware uses are
 * accepted: a Name whose object is the package, and a Method whose body is
 * `Return ( Package { ... } )`. */
static int aml_eval_prt(const aml_scope_t *s) {
    if (!s || !s->prt_obj)
        return -1;
    const uint8_t *p = s->prt_obj;
    const uint8_t *end = s->prt_obj + s->prt_len;

    if (p < end && *p == AML_RETURN) {
        p++;
        if (p >= end)
            return -1;
    }
    if (p >= end || *p != AML_PACKAGE)
        return -1;

    uint32_t len;
    p++;
    if (aml_pkg_length(&p, end, &len) != 0)
        return -1;
    if (aml_eval_package(&p, end, len, 0) != 0)
        return -1;
    return g_pkg_count[0] ? 0 : -1;
}

/*
 * Read one line out of an evaluated _PRT.
 *
 * Each package element is Package(address, mask, irq...), where the address is
 * the PCI slot address -- function in bits 4:0, device in bits 9:5 -- and the
 * mask selects which of the four INTx pins the following numbers describe.  A
 * mask of 0xFFFF marks an interrupt-source override, where the single number
 * that follows is a GSI rather than a pin index.  Both forms answer the same
 * question, which is why both are accepted.
 *
 * The INTx pins of a slot are shared by every function of it, so a routing
 * entry naming any function routes the line for all of them; the function
 * field is therefore matched but not used to narrow the answer.
 */
static uint32_t aml_prt_lookup(const aml_scope_t *s, int dev, int pin) {
    if (aml_eval_prt(s) != 0)
        return PCI_PRT_GSI_NONE;

    unsigned entries = g_pkg_count[0];
    for (unsigned i = 0; i < entries; i++) {
        if (!g_pkg[0][i].is_package)
            return PCI_PRT_GSI_NONE;     /* a flat package is not a _PRT */
        unsigned n = g_pkg_count[1];
        if (n < 3)
            continue;

        uint32_t address = (uint32_t)g_pkg[1][0].value;
        uint32_t mask    = (uint32_t)g_pkg[1][1].value;
        uint32_t slot_dev = (address >> 5) & 0x1FU;
        uint32_t src      = (uint32_t)pin - 1U;

        if (slot_dev != (uint32_t)dev)
            continue;

        uint32_t index;
        if (mask == 0xFFFFU) {
            /* One GSI for the whole override entry; it is the third element. */
            index = 2U;
        } else {
            /* Otherwise the numbers follow in the order of the mask bits, so
             * the pin's number sits after however many lower bits are set. */
            if (!(mask & (1U << src)))
                continue;
            uint32_t below = mask & ((1U << src) - 1U);
            index = 2U;
            while (below) {
                index += below & 1U;
                below >>= 1;
            }
        }
        if (index >= n)
            continue;
        return (uint32_t)g_pkg[1][index].value;
    }
    return PCI_PRT_GSI_NONE;
}

/*
 * Which scope describes @bus.
 *
 * Only the root bus is answered.  A secondary bus is named by the _ADR of the
 * bridge that leads to it, so reaching one means descending the bridge tree and
 * tracking which bus each level is on -- a walk this subset does not perform.
 * Claiming a scope whose _ADR happens to look like a bridge address would route
 * every bridge in the machine to every bus behind it, so anything but bus 0
 * returns no answer and the device keeps its polling fallback.
 */
static const aml_scope_t *dsdt_scope_for_bus(int bus) {
    if (bus != 0)
        return NULL;
    for (unsigned i = 0; i < g_scope_count; i++) {
        if (g_scope[i].has_adr && g_scope[i].adr == 0 && g_scope[i].prt_obj)
            return &g_scope[i];
    }
    /* A scope with a _PRT but no _ADR is still bus 0 on a firmware that leans
     * on the scope name rather than the address. */
    for (unsigned i = 0; i < g_scope_count; i++) {
        if (g_scope[i].prt_obj && strcmp(g_scope[i].path, "\\PCI0") == 0)
            return &g_scope[i];
    }
    return NULL;
}

uint32_t arch_pci_prt_gsi(int bus, int dev, int pin) {
    if (bus < 0 || dev < 0 || dev > 31 || pin < 1 || pin > 4)
        return PCI_PRT_GSI_NONE;

    dsdt_load();
    if (!dsdt_usable)
        return PCI_PRT_GSI_NONE;

    const aml_scope_t *scope = dsdt_scope_for_bus(bus);
    if (!scope)
        return PCI_PRT_GSI_NONE;
    return aml_prt_lookup(scope, dev, pin);
}

#endif /* CONFIG_X86_64 */