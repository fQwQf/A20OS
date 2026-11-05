/*
 * mkfs_lfs — host-side formatter for A20OS littlefs gate images.
 *
 * Links the vendored littlefs (kernel/external/littlefs) against the host
 * libc and writes a formatted image containing the fixed payload the guest
 * gate verifies: /hello.txt with known bytes plus a directory with a file.
 *
 *   mkfs_lfs <image> <size_mb>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lfs.h"

static FILE *g_img;

static int bd_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size) {
    (void)c;
    if (fseek(g_img, (long)((uint64_t)block * 4096 + off), SEEK_SET)) return LFS_ERR_IO;
    if (fread(buffer, 1, size, g_img) != size) return LFS_ERR_IO;
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size) {
    (void)c;
    if (fseek(g_img, (long)((uint64_t)block * 4096 + off), SEEK_SET)) return LFS_ERR_IO;
    if (fwrite(buffer, 1, size, g_img) != size) return LFS_ERR_IO;
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c; (void)block;
    return 0;
}

static int bd_sync(const struct lfs_config *c) {
    (void)c;
    return fflush(g_img) == 0 ? 0 : LFS_ERR_IO;
}

static const char *HELLO =
    "A20OS littlefs gate payload\n"
    "power-loss resilient by construction (pair-block commits)\n";

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <image> <size_mb>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    long mb = atol(argv[2]);
    if (mb < 1) mb = 1;

    g_img = fopen(path, "wb+");
    if (!g_img) { perror("fopen"); return 1; }
    if (ftruncate(fileno(g_img), mb * 1024L * 1024L)) { perror("ftruncate"); return 1; }

    struct lfs_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.context = NULL;
    cfg.read = bd_read;
    cfg.prog = bd_prog;
    cfg.erase = bd_erase;
    cfg.sync = bd_sync;
    cfg.read_size = 4096;
    cfg.prog_size = 4096;
    cfg.block_size = 4096;
    cfg.block_count = (uint32_t)((uint64_t)mb * 1024 * 1024 / 4096);
    cfg.block_cycles = 500;
    cfg.cache_size = 4096;
    cfg.lookahead_size = 8 * 4096;

    lfs_t lfs;
    int r = lfs_format(&lfs, &cfg);
    if (r) { fprintf(stderr, "format failed: %d\n", r); return 1; }
    r = lfs_mount(&lfs, &cfg);
    if (r) { fprintf(stderr, "mount failed: %d\n", r); return 1; }

    lfs_file_t f;
    if (lfs_file_open(&lfs, &f, "/hello.txt",
                      LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0) {
        fprintf(stderr, "open hello.txt failed\n");
        return 1;
    }
    lfs_file_write(&lfs, &f, HELLO, strlen(HELLO));
    lfs_file_close(&lfs, &f);

    if (lfs_mkdir(&lfs, "/data") < 0) { fprintf(stderr, "mkdir failed\n"); return 1; }
    if (lfs_file_open(&lfs, &f, "/data/seed.bin",
                      LFS_O_WRONLY | LFS_O_CREAT) < 0) {
        fprintf(stderr, "open data/seed.bin failed\n");
        return 1;
    }
    /* deterministic 4 KiB pattern the guest verifies byte-for-byte */
    unsigned char seed[4096];
    for (int i = 0; i < 4096; i++)
        seed[i] = (unsigned char)(i * 131 + 17);
    lfs_file_write(&lfs, &f, seed, sizeof(seed));
    lfs_file_close(&lfs, &f);

    lfs_unmount(&lfs);
    fclose(g_img);
    printf("mkfs_lfs: wrote %s (%ld MiB, hello.txt + data/seed.bin)\n",
           path, mb);
    return 0;
}
