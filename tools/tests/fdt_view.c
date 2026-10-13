/* Exercise the production bounded FDT reader under ASan/UBSan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/fdt_view.h"

#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);               \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static int open_and_walk(const unsigned char *p, size_t size) {
    fdt_view_t v;
    int rc = fdt_view_open(&v, p, size);
    if (rc)
        return rc;
    fdt_cursor_t c;
    fdt_event_t e;
    fdt_cursor_init(&c, &v);
    unsigned nodes = 0, properties = 0;
    while ((rc = fdt_cursor_next(&c, &e)) > 0) {
        if (e.type == FDT_NODE_BEGIN)
            nodes++;
        if (e.type == FDT_PROPERTY) {
            properties++;
            (void)fdt_string_list_has(&e, "brcm,bcm2711");
            (void)fdt_string_is(&e, "cpu");
        }
    }
    CHECK(rc == 0 && nodes && properties);
    uint64_t address, bytes;
    CHECK(fdt_view_reserve(&v, 0, &address, &bytes) == 1);
    CHECK(bytes > 0);
    CHECK(fdt_view_reserve(&v, 1, &address, &bytes) == 0);
    CHECK(fdt_view_reserve(&v, (size_t)-1, &address, &bytes) == -1);
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    CHECK(f != NULL && fseek(f, 0, SEEK_END) == 0);
    long length = ftell(f);
    CHECK(length >= 40 && length < 0x200000 && fseek(f, 0, SEEK_SET) == 0);
    size_t n = (size_t)length;
    unsigned char *p = malloc(n);
    CHECK(p && fread(p, 1, n, f) == n);
    fclose(f);
    CHECK(open_and_walk(p, n) == 0);
    /* Exact-sized copies make a read past the caller's mapping visible to
     * ASan, including every truncated header, node, property, and string. */
    for (size_t size = 0; size < n; size++) {
        unsigned char *short_blob = malloc(size ? size : 1);
        CHECK(short_blob != NULL);
        memcpy(short_blob, p, size);
        CHECK(open_and_walk(short_blob, size) == -1);
        free(short_blob);
    }
    unsigned char *mutant = malloc(n);
    CHECK(mutant != NULL);
    unsigned rejected = 0;
    for (size_t pos = 0; pos < n; pos++) {
        memcpy(mutant, p, n);
        mutant[pos] ^= 0xff;
        if (open_and_walk(mutant, n) != 0)
            rejected++;
    }
    /* Deliberately poison each header offset/size with UINT32_MAX. */
    const unsigned fields[] = {4, 8, 12, 16, 24, 32, 36};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        memcpy(mutant, p, n);
        memset(mutant + fields[i], 0xff, 4);
        CHECK(open_and_walk(mutant, n) == -1);
    }
    printf("PASS FDT bounds: %zu truncations, %zu byte mutations (%u "
           "rejected), 7 invalid headers\n",
           n, n, rejected);
    free(mutant);
    free(p);
    return 0;
}
