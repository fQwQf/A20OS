/* Canonical bounded ASCII word dictionaries. Exact counts, no truncation or
 * hashing collisions; the same operators run on a leaf and a hosted agent. */
#ifndef A20_CLUSTER_WORDS_H
#define A20_CLUSTER_WORDS_H
#include "cluster_work.h"
#define A20_WORK_WORD_MAP 3u
#define A20_WORK_WORD_MERGE 4u
#define A20_WORK_CONCAT 5u
#define A20_WORD_ENTRIES 8u
#define A20_WORD_BYTES 12u
#define A20_WORD_RESULT (4u + A20_WORD_ENTRIES * 16u)
#define A20_WORD_MERGE_INPUT (8u + 2u * A20_WORD_RESULT)
static inline int a20_word_char(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}
static inline int a20_word_order(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < A20_WORD_BYTES; i++) {
        if (a[i] < b[i])
            return -1;
        if (a[i] > b[i])
            return 1;
    }
    return 0;
}
static inline int a20_word_dictionary_valid(const uint8_t *p, uint32_t len) {
    if (!p || len < 4 || a20_work_u32(p) > A20_WORD_ENTRIES ||
        len != 4 + a20_work_u32(p) * 16)
        return 0;
    for (unsigned i = 0; i < a20_work_u32(p); i++) {
        const uint8_t *word = p + 4 + i * 16;
        unsigned ended = 0;
        if (!word[0] || !a20_work_u32(word + 12))
            return 0;
        for (unsigned k = 0; k < A20_WORD_BYTES; k++) {
            if (!word[k])
                ended = 1;
            else if (ended || !((word[k] >= 'a' && word[k] <= 'z') ||
                                (word[k] >= '0' && word[k] <= '9')))
                return 0;
        }
        if (i && a20_word_order(word - 16, word) >= 0)
            return 0;
    }
    return 1;
}
static inline uint32_t a20_word_add(uint8_t *out, const uint8_t word[12],
                                    uint32_t amount) {
    unsigned count = a20_work_u32(out), i = 0;
    while (i < count && a20_word_order(out + 4 + i * 16, word) < 0)
        i++;
    uint8_t *entry = out + 4 + i * 16;
    if (i < count && !a20_word_order(entry, word)) {
        uint32_t old = a20_work_u32(entry + 12);
        if (amount > UINT32_MAX - old)
            return A20_WORK_NO_SPACE;
        a20_work_put32(entry + 12, old + amount);
    } else {
        if (count == A20_WORD_ENTRIES)
            return A20_WORK_NO_SPACE;
        for (unsigned j = count * 16; j > i * 16; j--)
            out[4 + j + 15] = out[4 + j - 1];
        for (unsigned j = 0; j < 12; j++)
            entry[j] = word[j];
        a20_work_put32(entry + 12, amount);
        a20_work_put32(out, count + 1);
    }
    return A20_WORK_OK;
}
static inline uint32_t a20_work_word_map(void *ctx, const a20_work_request_t *r,
                                         uint8_t *out, uint32_t *n) {
    (void)ctx;
    if (*n < A20_WORD_RESULT || (r->input_len && !r->input))
        return A20_WORK_INVALID;
    a20_work_put32(out, 0);
    unsigned at = 0;
    while (at < r->input_len) {
        uint8_t ch = r->input[at];
        if (!ch || ch > 127)
            return A20_WORK_INVALID;
        if (!a20_word_char(ch)) {
            at++;
            continue;
        }
        uint8_t word[12] = {0};
        unsigned used = 0;
        while (at < r->input_len && a20_word_char(r->input[at])) {
            if (used == A20_WORD_BYTES)
                return A20_WORK_NO_SPACE;
            ch = r->input[at++];
            word[used++] = ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
        }
        uint32_t status = a20_word_add(out, word, 1);
        if (status)
            return status;
    }
    *n = 4 + a20_work_u32(out) * 16;
    return A20_WORK_OK;
}
/* Reduction input is a sequence of u32 byte lengths followed by child bytes.
 * Framing is generic; operators independently validate each child's schema. */
static inline uint32_t a20_work_word_merge(void *ctx,
                                           const a20_work_request_t *r,
                                           uint8_t *out, uint32_t *n) {
    (void)ctx;
    if (*n < A20_WORD_RESULT || !r->input || !r->input_len)
        return A20_WORK_INVALID;
    a20_work_put32(out, 0);
    unsigned at = 0, parts = 0;
    while (at < r->input_len) {
        if (r->input_len - at < 4 || ++parts > 2)
            return A20_WORK_INVALID;
        uint32_t len = a20_work_u32(r->input + at);
        at += 4;
        if (len > r->input_len - at ||
            !a20_word_dictionary_valid(r->input + at, len))
            return A20_WORK_INVALID;
        const uint8_t *p = r->input + at;
        for (unsigned i = 0; i < a20_work_u32(p); i++) {
            uint32_t status = a20_word_add(out, p + 4 + i * 16,
                                           a20_work_u32(p + 16 + i * 16));
            if (status)
                return status;
        }
        at += len;
    }
    *n = 4 + a20_work_u32(out) * 16;
    return A20_WORK_OK;
}
static inline uint32_t a20_work_concat(void *ctx, const a20_work_request_t *r,
                                       uint8_t *out, uint32_t *n) {
    (void)ctx;
    if (!r->input || !r->input_len)
        return A20_WORK_INVALID;
    uint32_t at = 0, used = 0, parts = 0;
    while (at < r->input_len) {
        if (r->input_len - at < 4 || ++parts > 2)
            return A20_WORK_INVALID;
        uint32_t len = a20_work_u32(r->input + at);
        at += 4;
        if (len > r->input_len - at)
            return A20_WORK_INVALID;
        if (len > *n - used)
            return A20_WORK_NO_SPACE;
        for (unsigned i = 0; i < len; i++)
            out[used + i] = r->input[at + i];
        used += len;
        at += len;
    }
    *n = used;
    return A20_WORK_OK;
}
#endif
