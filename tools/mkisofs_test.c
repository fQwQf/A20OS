/*
 * A20OS — ISO9660 test image generator.
 *
 * Builds a read-only ISO exercising the shapes the driver has to get right,
 * not just the happy path:
 *
 *   - a root directory spanning several 2048-byte blocks (a record may not
 *     cross a block boundary; the slack is zero padding and the walk has to
 *     resume at the next block),
 *   - a 200-byte file name, near the driver's ISOFS_MAX_NAME limit,
 *   - a zero-length file, a file of exactly one block, and a file spanning
 *     several blocks,
 *   - a nested directory.
 *
 * No Rock Ridge / Joliet: names are plain uppercase, translated to lowercase
 * by the driver.
 *
 * Usage: mkisofs_test <output>
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define BS 2048U

/* Enough filler files that the root directory cannot fit in one block. */
#define FILLER_COUNT 150
#define LONG_NAME_LEN 200
#define BIG_SIZE 9000U
#define DIR_MAX_BLOCKS 64U

static const char hello_content[] = "hello iso9660 world\n";
static const char nested_content[] = "nested file content\n";
static const char long_content[] = "long-name payload\n";

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_both16(uint8_t *p, uint16_t v) { put_le16(p, v); put_be16(p + 2, v); }
static void put_both32(uint8_t *p, uint32_t v) { put_le32(p, v); put_be32(p + 4, v); }

/* Byte offsets inside a directory record (ISO 9660 9.1). */
#define REC_LEN        0
#define REC_EXTENT     2
#define REC_SIZE       10
#define REC_FLAGS      25
#define REC_NAME_LEN   32
#define REC_NAME       33

/* Build a directory record into buf; returns its padded length. */
static int mk_dir_record(uint8_t *buf, const uint8_t *name, int nlen,
                         uint32_t lba, uint32_t size, int is_dir) {
    int rec_len = REC_NAME + nlen;
    if (rec_len % 2) rec_len++;
    memset(buf, 0, (size_t)rec_len);
    buf[REC_LEN] = (uint8_t)rec_len;
    put_both32(buf + REC_EXTENT, lba);
    put_both32(buf + REC_SIZE, size);
    buf[REC_FLAGS] = (uint8_t)(is_dir ? 2 : 0);
    put_both16(buf + 28, 1);
    buf[REC_NAME_LEN] = (uint8_t)nlen;
    if (nlen > 0) memcpy(buf + REC_NAME, name, (size_t)nlen);
    return rec_len;
}

typedef struct {
    uint8_t *buf;
    size_t   used;
} dirimg_t;

/*
 * Append a record.  ISO 9660 requires a record to lie wholly inside one
 * logical block, so a record that does not fit pads the rest of the current
 * block with zeros and starts the next one.  Returns the record's byte offset
 * so self/parent references can be patched once every size is known.
 */
static size_t dir_add(dirimg_t *d, const uint8_t *name, int nlen,
                      uint32_t lba, uint32_t size, int is_dir) {
    int rec_len = REC_NAME + nlen;
    if (rec_len % 2) rec_len++;
    size_t in_block = d->used % BS;
    if (in_block + (size_t)rec_len > BS)
        d->used += BS - in_block;
    size_t off = d->used;
    mk_dir_record(d->buf + off, name, nlen, lba, size, is_dir);
    d->used += (size_t)rec_len;
    return off;
}

/* The name a filler file gets, shared by the generator and the guest test. */
static void filler_name(int i, char *out, size_t out_sz) {
    snprintf(out, out_sz, "F%03d.TXT", i);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output>\n", argv[0]);
        return 1;
    }

    dirimg_t root = { calloc(DIR_MAX_BLOCKS, BS), 0 };
    dirimg_t sub = { calloc(DIR_MAX_BLOCKS, BS), 0 };
    if (!root.buf || !sub.buf) return 1;

    char long_name[LONG_NAME_LEN + 1];
    memset(long_name, 'L', sizeof(long_name));
    memcpy(long_name, "LONGNAME_", 9);
    memcpy(long_name + LONG_NAME_LEN - 4, ".TXT", 4);
    long_name[LONG_NAME_LEN] = '\0';

    /* ---- pass 1: root entries; self/parent/file extents patched later */
    size_t root_dot = dir_add(&root, (const uint8_t *)"", 1, 0, 0, 1);
    size_t root_dotdot = dir_add(&root, (const uint8_t *)"\1", 1, 0, 0, 1);
    dir_add(&root, (const uint8_t *)"HELLO.TXT", 9, 0, 0, 0);
    size_t root_sub = dir_add(&root, (const uint8_t *)"SUB", 3, 0, 0, 1);
    dir_add(&root, (const uint8_t *)"ZERO.BIN", 8, 0, 0, 0);
    dir_add(&root, (const uint8_t *)"BLOCK.BIN", 9, 0, 0, 0);
    dir_add(&root, (const uint8_t *)"BIG.BIN", 7, 0, 0, 0);
    dir_add(&root, (const uint8_t *)long_name, LONG_NAME_LEN, 0, 0, 0);
    for (int i = 0; i < FILLER_COUNT; i++) {
        char name[16];
        filler_name(i, name, sizeof(name));
        dir_add(&root, (const uint8_t *)name, (int)strlen(name), 0, 0, 0);
    }
    uint32_t root_size = (uint32_t)root.used;
    uint32_t root_blocks = (uint32_t)((root.used + BS - 1) / BS);

    uint32_t root_lba = 18;
    uint32_t sub_lba = root_lba + root_blocks;

    /* ---- pass 2: SUB entries */
    size_t sub_dot = dir_add(&sub, (const uint8_t *)"", 1, 0, 0, 1);
    size_t sub_dotdot = dir_add(&sub, (const uint8_t *)"\1", 1, 0, 0, 1);
    size_t sub_nested = dir_add(&sub, (const uint8_t *)"NESTED.TXT", 10, 0, 0, 0);
    uint32_t sub_size = (uint32_t)sub.used;
    uint32_t sub_blocks = (uint32_t)((sub.used + BS - 1) / BS);

    /* ---- contiguous file data after the directories */
    uint32_t data_lba = sub_lba + sub_blocks;
    uint32_t hello_lba = data_lba++;
    uint32_t zero_lba = 0;                    /* zero length: LBA unused */
    uint32_t block_lba = data_lba; data_lba += 1;            /* exactly BS */
    uint32_t big_lba = data_lba; data_lba += (BIG_SIZE + BS - 1) / BS;
    uint32_t long_lba = data_lba++;
    uint32_t nested_lba = data_lba++;
    uint32_t filler_base = data_lba;
    data_lba += FILLER_COUNT;

    uint32_t total = data_lba;

    /* ---- patch deferred records now that every size and LBA is known */
    mk_dir_record(root.buf + root_dot, (const uint8_t *)"", 1, root_lba,
                  root_size, 1);
    mk_dir_record(root.buf + root_dotdot, (const uint8_t *)"\1", 1, root_lba,
                  root_size, 1);
    mk_dir_record(root.buf + root_sub, (const uint8_t *)"SUB", 3, sub_lba,
                  sub_size, 1);
    mk_dir_record(sub.buf + sub_dot, (const uint8_t *)"", 1, sub_lba, sub_size, 1);
    mk_dir_record(sub.buf + sub_dotdot, (const uint8_t *)"\1", 1, root_lba,
                  root_size, 1);
    put_le32(sub.buf + sub_nested + REC_EXTENT, nested_lba);
    put_be32(sub.buf + sub_nested + REC_EXTENT + 4, nested_lba);
    put_le32(sub.buf + sub_nested + REC_SIZE,
             (uint32_t)(sizeof(nested_content) - 1));
    put_be32(sub.buf + sub_nested + REC_SIZE + 4,
             (uint32_t)(sizeof(nested_content) - 1));

    /* Walk the root records once to fill in each file's extent and size. */
    {
        char name[LONG_NAME_LEN + 1];
        struct { const char *name; uint32_t lba; uint32_t size; } fixed[] = {
            { "HELLO.TXT", hello_lba, (uint32_t)(sizeof(hello_content) - 1) },
            { "ZERO.BIN", zero_lba, 0 },
            { "BLOCK.BIN", block_lba, BS },
            { "BIG.BIN", big_lba, BIG_SIZE },
            { long_name, long_lba, (uint32_t)(sizeof(long_content) - 1) },
        };
        for (size_t off = 0; off < root.used; ) {
            uint8_t len = root.buf[off + REC_LEN];
            if (len == 0) { off += BS - (off % BS); continue; }
            int nlen = root.buf[off + REC_NAME_LEN];
            memcpy(name, root.buf + off + REC_NAME, (size_t)nlen);
            name[nlen] = '\0';
            for (size_t f = 0; f < sizeof(fixed) / sizeof(fixed[0]); f++) {
                if (strcmp(name, fixed[f].name) == 0) {
                    put_le32(root.buf + off + REC_EXTENT, fixed[f].lba);
                    put_be32(root.buf + off + REC_EXTENT + 4, fixed[f].lba);
                    put_le32(root.buf + off + REC_SIZE, fixed[f].size);
                    put_be32(root.buf + off + REC_SIZE + 4, fixed[f].size);
                }
            }
            if (nlen == 8 && name[0] == 'F' && name[4] == '.') {
                /* filler Fnnn.TXT: one block each, holding its own index. */
                int idx = (name[1] - '0') * 100 + (name[2] - '0') * 10 +
                          (name[3] - '0');
                uint32_t lba = filler_base + (uint32_t)idx;
                put_le32(root.buf + off + REC_EXTENT, lba);
                put_be32(root.buf + off + REC_EXTENT + 4, lba);
                char filler[16];
                uint32_t sz = (uint32_t)strlen(
                    (snprintf(filler, sizeof(filler), "filler-%03d\n", idx),
                     filler));
                put_le32(root.buf + off + REC_SIZE, sz);
                put_be32(root.buf + off + REC_SIZE + 4, sz);
            }
            off += len;
        }
    }

    /* ---- image assembly */
    size_t img_sz = (size_t)total * BS;
    uint8_t *img = calloc(1, img_sz);
    if (!img) { free(root.buf); free(sub.buf); return 1; }

    uint8_t *pvd = img + 16U * BS;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memset(pvd + 8, ' ', 32);      /* system id */
    memset(pvd + 40, ' ', 32);     /* volume id */
    put_both32(pvd + 80, total);   /* volume space size */
    put_both16(pvd + 120, 1);      /* volume set size */
    put_both16(pvd + 124, 1);      /* volume seq */
    put_both16(pvd + 128, BS);     /* logical block size */
    mk_dir_record(pvd + 156, (const uint8_t *)"", 1, root_lba, root_size, 1);

    uint8_t *term = img + 17U * BS;
    term[0] = 255;
    memcpy(term + 1, "CD001", 5);
    term[6] = 1;

    memcpy(img + (size_t)root_lba * BS, root.buf, root.used);
    memcpy(img + (size_t)sub_lba * BS, sub.buf, sub.used);

    memcpy(img + (size_t)hello_lba * BS, hello_content, sizeof(hello_content) - 1);
    memcpy(img + (size_t)long_lba * BS, long_content, sizeof(long_content) - 1);
    memcpy(img + (size_t)nested_lba * BS, nested_content, sizeof(nested_content) - 1);
    for (uint32_t off = 0; off < BS; off++)
        img[(size_t)block_lba * BS + off] = (uint8_t)(off % 251);
    for (uint32_t off = 0; off < BIG_SIZE; off++)
        img[(size_t)big_lba * BS + off] = (uint8_t)('A' + (off % 26));
    for (int i = 0; i < FILLER_COUNT; i++) {
        char *p = (char *)img + (size_t)(filler_base + (uint32_t)i) * BS;
        snprintf(p, 16, "filler-%03d\n", i);
    }

    FILE *f = fopen(argv[1], "wb");
    if (!f) { free(img); free(root.buf); free(sub.buf); return 1; }
    fwrite(img, 1, img_sz, f);
    fclose(f);
    free(img);
    free(root.buf);
    free(sub.buf);
    printf("mkisofs_test: wrote %zu bytes to %s (root dir %u bytes in %u blocks)\n",
           img_sz, argv[1], root_size, root_blocks);
    return 0;
}
