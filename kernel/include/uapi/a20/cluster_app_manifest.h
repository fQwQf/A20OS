#ifndef A20_CLUSTER_APP_MANIFEST_H
#define A20_CLUSTER_APP_MANIFEST_H
#include "cluster_apps.h"
#include "abi_features.h"
/* Owner-local deployment input. Mutable backing bytes and entries must outlive
 * all launches. Pointers never cross the cluster wire. No allocation or ISA
 * policy: the same parsed entry is usable by a firmware or Native adapter. */
#define A20_APP_MANIFEST_BYTES 16384u
#define A20_APP_MANIFEST_ARGC 16u
#define A20_APP_MANIFEST_ENVC 16u
typedef struct a20_app_manifest_entry {
    a20_app_variant_t variant;
    const char *argv[A20_APP_MANIFEST_ARGC + 1];
    const char *envp[A20_APP_MANIFEST_ENVC + 1];
    uint32_t argc, envc;
    uint64_t required_abi_features[4];
} a20_app_manifest_entry_t;
static inline int
a20_app_manifest_features_match(const a20_app_manifest_entry_t *entry,
                                const uint64_t supported[4]) {
    for (unsigned i = 0; i < 4; i++)
        if (entry->required_abi_features[i] & ~supported[i])
            return 0;
    return 1;
}
static inline int a20_app_text_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
static inline int a20_app_decimal(const char *s, uint32_t *out) {
    uint32_t value = 0;
    if (!s || !*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' ||
            value > (UINT32_MAX - (unsigned)(*s - '0')) / 10)
            return -1;
        value = value * 10 + (unsigned)(*s - '0');
    }
    *out = value;
    return 0;
}
static inline int a20_app_manifest_uuid(const char *s, uint8_t out[16]) {
    for (unsigned i = 0; i < 16; i++) {
        unsigned byte = 0;
        for (unsigned j = 0; j < 2; j++) {
            unsigned char c = *s++;
            unsigned d = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
            if (d == 16)
                return -1;
            byte = byte * 16 + d;
        }
        out[i] = (uint8_t)byte;
    }
    return *s || !a20_runtime_id_valid(out) ? -1 : 0;
}
/* Exactly eight whitespace-delimited words; argument/env lines are separate
 * literal lines and are not split, unescaped or expanded. */
static inline int a20_app_manifest_header(char *line,
                                          a20_app_manifest_entry_t *e) {
    char *words[8];
    unsigned count = 0;
    while (*line) {
        if (count == 8)
            return -1;
        words[count++] = line;
        while (*line && *line != ' ' && *line != '\t')
            line++;
        if (!*line)
            break;
        *line++ = 0;
        while (*line == ' ' || *line == '\t')
            line++;
    }
    if (count != 8 || !a20_app_text_equal(words[0], "app") ||
        a20_app_manifest_uuid(words[1], e->variant.manifest) ||
        words[2][0] != '/')
        return -1;
    a20_execution_requirement_t *v = &e->variant.requirement;
    if (a20_app_decimal(words[3], &v->elf_machine) || !v->elf_machine ||
        a20_app_decimal(words[4], &v->pointer_bits) ||
        (v->pointer_bits != 32 && v->pointer_bits != 64) ||
        a20_app_decimal(words[5], &v->byte_order) ||
        (v->byte_order != 1 && v->byte_order != 2) ||
        a20_app_decimal(words[6], &v->calling_abi) || !v->calling_abi ||
        v->calling_abi > A20_EXEC_ABI_MAX ||
        a20_app_decimal(words[7], &v->required_features) ||
        v->required_features & ~A20_EXEC_FEATURE_MASK)
        return -1;
    v->native_major = 1;
    e->argv[0] = words[2];
    e->argc = 1;
    e->variant.image = (uintptr_t)e;
    return 0;
}
/* bytes has length+1 writable storage. Reject NUL, oversized input, malformed
 * entries and unfinished blocks before publishing count. Empty args survive.
 * Entry/buffer contents on failure are scratch and must not be launched. */
static inline int a20_app_manifest_parse(char *bytes, uint32_t length,
                                         a20_app_manifest_entry_t *entries,
                                         uint32_t capacity, uint32_t *count) {
    if (!count)
        return -1;
    *count = 0;
    if (!bytes || !entries || !capacity || !length ||
        length > A20_APP_MANIFEST_BYTES)
        return -1;
    for (uint32_t i = 0; i < length; i++)
        if (!bytes[i])
            return -1;
    bytes[length] = 0;
    uint32_t used = 0;
    a20_app_manifest_entry_t *current = 0;
    char *line = bytes, *end = bytes + length;
    int first = 1;
    while (line < end) {
        char *next = line;
        while (next < end && *next != '\n')
            next++;
        char *tail = next;
        if (tail > line && tail[-1] == '\r')
            tail--;
        *tail = 0;
        next = next < end ? next + 1 : end;
        if (first) {
            if (!a20_app_text_equal(line, "A20APPS1"))
                return -1;
            first = 0;
        } else if (!*line || *line == '#') {
            /* blank/comment lines */
        } else if (!current) {
            if (used == capacity)
                return -1;
            current = &entries[used];
            *current = (a20_app_manifest_entry_t){0};
            if (a20_app_manifest_header(line, current))
                return -1;
        } else if (line[0] == 'a' && line[1] == 'r' && line[2] == 'g' &&
                   line[3] == ' ') {
            if (current->argc == A20_APP_MANIFEST_ARGC)
                return -1;
            current->argv[current->argc++] = line + 4;
        } else if (line[0] == 'e' && line[1] == 'n' && line[2] == 'v' &&
                   line[3] == ' ') {
            if (current->envc == A20_APP_MANIFEST_ENVC)
                return -1;
            const char *value = line + 4, *p = value;
            while (*p && *p != '=')
                p++;
            if (p == value || *p != '=')
                return -1;
            current->envp[current->envc++] = value;
        } else if (line[0] == 'r' && line[1] == 'e' && line[2] == 'q' &&
                   line[3] == 'u' && line[4] == 'i' && line[5] == 'r' &&
                   line[6] == 'e' && line[7] == '-' && line[8] == 'f' &&
                   line[9] == 'e' && line[10] == 'a' && line[11] == 't' &&
                   line[12] == 'u' && line[13] == 'r' && line[14] == 'e' &&
                   line[15] == ' ') {
            uint32_t feature;
            if (a20_app_decimal(line + 16, &feature) ||
                feature >= A20_ABI_FEATURE_COUNT ||
                (current->required_abi_features[feature / 64] &
                 (1ULL << (feature % 64))))
                return -1;
            current->required_abi_features[feature / 64] |= 1ULL
                                                            << (feature % 64);
        } else if (a20_app_text_equal(line, "end")) {
            used++;
            current = 0;
        } else
            return -1;
        line = next;
    }
    if (current || !used)
        return -1;
    *count = used;
    return 0;
}
#endif
