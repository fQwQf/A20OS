/*
 * A20OS — minimal ISO9660 test image generator.
 *
 * Builds a small read-only ISO with two files (one nested) so the isofs
 * driver can be exercised in QEMU without requiring mkisofs/xorriso on the
 * build host.  No Rock Ridge / Joliet: names are plain uppercase.
 *
 * Usage: mkisofs_test <output>
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define BS 2048U

struct file_ent {
    const char *path;          /* "DIR/FILE.TXT" */
    const char *content;
};

static const struct file_ent g_files[] = {
    { "HELLO.TXT", "hello iso9660 world\n" },
    { "SUB/NESTED.TXT", "nested file content\n" },
};

#define NFILES ((int)(sizeof(g_files) / sizeof(g_files[0])))

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

/* Build a directory record into buf; returns total length (incl. padding). */
static int mk_dir_record(uint8_t *buf, const char *name, uint32_t lba,
                         uint32_t size, int is_dir) {
    int nlen = (int)strlen(name);
    int rec_len = 33 + nlen;
    if (rec_len % 2) rec_len++;
    memset(buf, 0, (size_t)rec_len);
    buf[0] = (uint8_t)rec_len;
    put_both32(buf + 2, lba);
    put_both32(buf + 10, size);
    buf[25] = (uint8_t)(is_dir ? 2 : 0);
    put_both16(buf + 28, 1);
    buf[32] = (uint8_t)nlen;
    memcpy(buf + 33, name, (size_t)nlen);
    return rec_len;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output>\n", argv[0]);
        return 1;
    }

    /* Two directories: root ("") and "SUB".  Files: HELLO.TXT (root),
     * SUB/NESTED.TXT. */

    /* Layout: PVD @16, terminator @17, dirs @18(root)/19(SUB), data @20+. */
    uint32_t root_lba = 18;
    uint32_t sub_lba = 19;
    uint32_t hello_lba = 20;
    uint32_t nested_lba = 21;
    uint32_t total = 22;

    size_t img_sz = (size_t)total * BS;
    uint8_t *img = calloc(1, img_sz);
    if (!img) return 1;

    /* --- PVD at LBA 16 --- */
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
    /* Root directory record at offset 156 (34 bytes). */
    {
        uint8_t *rr = pvd + 156;
        int n = mk_dir_record(rr, "\0", root_lba, 0, 1);
        (void)n;
    }

    /* --- Terminator at LBA 17 --- */
    uint8_t *term = img + 17U * BS;
    term[0] = 255;
    memcpy(term + 1, "CD001", 5);
    term[6] = 1;

    /* --- Root directory at LBA 18 --- */
    uint8_t *rd = img + (size_t)root_lba * BS;
    uint32_t root_used = 0;
    {
        int off = 0;
        off += mk_dir_record(rd + off, "\0", root_lba, 0, 1);        /* . */
        off += mk_dir_record(rd + off, "\1", root_lba, 0, 1);        /* .. */
        off += mk_dir_record(rd + off, "HELLO.TXT", hello_lba,
                             (uint32_t)strlen(g_files[0].content), 0);
        off += mk_dir_record(rd + off, "SUB", sub_lba, 0, 1);
        root_used = (uint32_t)off;
    }

    /* --- SUB directory at LBA 19 --- */
    uint8_t *sd = img + (size_t)sub_lba * BS;
    uint32_t sub_used = 0;
    {
        int off = 0;
        off += mk_dir_record(sd + off, "\0", sub_lba, 0, 1);         /* . */
        off += mk_dir_record(sd + off, "\1", root_lba, 0, 1);        /* .. */
        off += mk_dir_record(sd + off, "NESTED.TXT", nested_lba,
                             (uint32_t)strlen(g_files[1].content), 0);
        sub_used = (uint32_t)off;
    }

    /* Fill the real directory sizes back into the PVD root record and the
     * root's "SUB" record, and the root "." / SUB "." self-references. */
    {
        /* PVD root record at offset 156: bytes 10..17 are size (733). */
        put_le32(pvd + 156 + 10, root_used);
        put_be32(pvd + 156 + 14, root_used);
        /* Root "." record is first in the root block. */
        put_le32(rd + 10, root_used);
        put_be32(rd + 14, root_used);
        /* Root ".." also references root. */
        put_le32(rd + 2, root_lba);
        put_be32(rd + 6, root_lba);
        /* SUB "." record in the SUB block. */
        put_le32(sd + 10, sub_used);
        put_be32(sd + 14, sub_used);
        /* Root's "SUB" record size (bytes 10..17) — locate by scanning. */
        for (int off = 0; off < (int)BS - 34; ) {
            uint8_t len = rd[off];
            if (len == 0) break;
            int nlen = rd[off + 32];
            if (nlen == 3 && memcmp(rd + off + 33, "SUB", 3) == 0) {
                put_le32(rd + off + 10, sub_used);
                put_be32(rd + off + 14, sub_used);
                break;
            }
            off += len;
        }
    }

    /* --- File data --- */
    memcpy(img + (size_t)hello_lba * BS, g_files[0].content,
           strlen(g_files[0].content));
    memcpy(img + (size_t)nested_lba * BS, g_files[1].content,
           strlen(g_files[1].content));

    FILE *f = fopen(argv[1], "wb");
    if (!f) { free(img); return 1; }
    fwrite(img, 1, img_sz, f);
    fclose(f);
    free(img);
    printf("mkisofs_test: wrote %zu bytes to %s\n", img_sz, argv[1]);
    return 0;
}
