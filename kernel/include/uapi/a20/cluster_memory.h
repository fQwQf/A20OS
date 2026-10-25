/* CMEM/1: explicitly refreshed remote pages, not a coherent mmap interface.
 * All wire integers are little endian. IDs fence service process restarts. */
#ifndef A20_CLUSTER_MEMORY_H
#define A20_CLUSTER_MEMORY_H
#include "cluster_config.h"
#include "cluster_work.h"
#ifndef A20_MEMORY_MAX_PAGES
#define A20_MEMORY_MAX_PAGES 8u
#endif
#define A20_MEMORY_SERVICE "cluster.memory"
#define A20_MEMORY_PAGE 4096u
#define A20_MEMORY_CHUNK 512u
#define A20_MEMORY_HEADER 192u
#define A20_MEMORY_MESSAGE (A20_MEMORY_HEADER + A20_MEMORY_CHUNK)
#define A20_MEMORY_STORAGE                                                     \
    (A20_MEMORY_MAX_PAGES * (A20_MEMORY_PAGE + 32u) + 8192u)
#define A20_MEMORY_OFF 0u
#define A20_MEMORY_READ_ONLY 1u
#define A20_MEMORY_READ_WRITE 2u
#define A20_MEMORY_HELLO 1u
#define A20_MEMORY_READ 2u
#define A20_MEMORY_BEGIN 3u
#define A20_MEMORY_STAGE 4u
#define A20_MEMORY_COMMIT 5u
#define A20_MEMORY_ABORT 6u
#define A20_MEMORY_OK 0u
#define A20_MEMORY_INVALID 1u
#define A20_MEMORY_DISABLED 2u
#define A20_MEMORY_DENIED 3u
#define A20_MEMORY_STALE 4u
#define A20_MEMORY_CONFLICT 5u
#define A20_MEMORY_BUSY 6u
#define A20_MEMORY_THROTTLED 7u
#define A20_MEMORY_INCOMPLETE 8u
#define A20_MEMORY_EXHAUSTED 9u

typedef struct a20_memory_config {
    uint32_t mode, pages, transaction_ms, bytes_per_second;
} a20_memory_config_t;

/* Strict and transactional. Omitted mode is OFF. Limits do not enable it.
 * Runtime page count caps usage; compile-time MAX_PAGES caps static storage. */
static inline int a20_memory_config_parse_file(
    const char *text, uint32_t len, a20_memory_config_t *out,
    char initial_file[A20_CONFIG_PATH], uint32_t *error_line) {
    a20_memory_config_t c = {0, A20_MEMORY_MAX_PAGES, 5000, 0};
    char initial[A20_CONFIG_PATH] = {0};
    uint32_t pos = 0, line_no = 0, seen = 0;
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
            line[n++] = (char)ch;
        }
        if (pos < len)
            pos++;
        line[n] = 0;
        char *words[3] = {0};
        unsigned count = 0, i = 0;
        while (i < n) {
            while (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')
                i++;
            if (!line[i] || line[i] == '#')
                break;
            if (count == 3)
                goto bad;
            words[count++] = line + i;
            while (line[i] && line[i] != ' ' && line[i] != '\t' &&
                   line[i] != '\r')
                i++;
            if (line[i])
                line[i++] = 0;
        }
        if (!count)
            continue;
        if (count != 2)
            goto bad;
        uint32_t bit = 0;
        uint64_t number;
        if (a20_config_equal(words[0], "version")) {
            bit = 1;
            if (!a20_config_equal(words[1], "1"))
                goto bad;
        } else if (a20_config_equal(words[0], "mode")) {
            bit = 2;
            if (a20_config_equal(words[1], "off"))
                c.mode = A20_MEMORY_OFF;
            else if (a20_config_equal(words[1], "read-only"))
                c.mode = A20_MEMORY_READ_ONLY;
            else if (a20_config_equal(words[1], "read-write"))
                c.mode = A20_MEMORY_READ_WRITE;
            else
                goto bad;
        } else if (a20_config_equal(words[0], "max-pages")) {
            bit = 4;
            if (a20_config_epoch(words[1], &number) ||
                number > A20_MEMORY_MAX_PAGES)
                goto bad;
            c.pages = (uint32_t)number;
        } else if (a20_config_equal(words[0], "transaction-ms")) {
            bit = 8;
            if (a20_config_epoch(words[1], &number) || number > 30000)
                goto bad;
            c.transaction_ms = (uint32_t)number;
        } else if (a20_config_equal(words[0], "bytes-per-second")) {
            bit = 16;
            if (a20_config_equal(words[1], "0"))
                c.bytes_per_second = 0;
            else {
                if (a20_config_epoch(words[1], &number) ||
                    number < A20_MEMORY_CHUNK || number > UINT32_MAX)
                    goto bad;
                c.bytes_per_second = (uint32_t)number;
            }
        } else if (a20_config_equal(words[0], "initial-file")) {
            bit = 32;
            if (!initial_file || a20_config_path(initial, words[1]))
                goto bad;
        } else
            goto bad;
        if (seen & bit)
            goto bad;
        seen |= bit;
    }
    if (!(seen & 1))
        goto bad;
    *out = c;
    if (initial_file)
        for (unsigned i = 0; i < A20_CONFIG_PATH; i++)
            initial_file[i] = initial[i];
    return 0;
bad:
    if (error_line)
        *error_line = line_no;
    return -1;
}
static inline int a20_memory_config_parse(const char *text, uint32_t len,
                                          a20_memory_config_t *out,
                                          uint32_t *error_line) {
    return a20_memory_config_parse_file(text, len, out, 0, error_line);
}

/* Server FFI. Storage must be 8-byte aligned, initialized once before
 * publication, and live until all dispatchers have stopped. Buffers cannot
 * overlap owner storage. Dispatch itself may run concurrently (returns BUSY).
 * owner is a validated, encoded 64-byte TASK object identity. */
int a20_memory_owner_init(void *storage, uint32_t size,
                          const a20_memory_config_t *config,
                          const uint8_t owner[64]);
int a20_memory_owner_dispatch(void *storage, const uint8_t *request,
                              uint32_t len, uint8_t *reply, uint32_t capacity,
                              uint64_t now_ms);
/* Initialization-only; do not call once the service is published. */
int a20_memory_owner_seed(void *storage, uint32_t page, const uint8_t *bytes,
                          uint32_t len);

/* Requests/replies: magic "MP", version=1, op; status/reserved@4;
 * cookie@8, page@16, offset@20, length@24, reserved@28;
 * version@32, transaction token@40, nonce@48, reserved@56;
 * owner object@64, client object@128; optional chunk@192.
 * HELLO response replaces page/offset/length/reserved@28 with
 * mode/pages/transaction_ms/bytes_per_second. */
static inline void a20_memory_request(uint8_t *q, uint32_t op, uint64_t cookie,
                                      const uint8_t owner[64],
                                      const uint8_t client[64]) {
    for (unsigned i = 0; i < A20_MEMORY_HEADER; i++)
        q[i] = 0;
    q[0] = 'M';
    q[1] = 'P';
    q[2] = 1;
    q[3] = (uint8_t)op;
    a20_work_put64(q + 8, cookie);
    for (unsigned i = 0; i < 64; i++) {
        q[64 + i] = owner ? owner[i] : 0;
        q[128 + i] = client ? client[i] : 0;
    }
}
#endif
