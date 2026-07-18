/*
 * TF-card FAT32 mount + convenience helpers. Binds fat32lite.c to the SDIO sector
 * driver. See sdfs.h.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "sdfs.h"
#include "core/sync.h"
#include "hub_cfg.h"
#include "hub_log.h"
#include "drivers/stm32f1/sdcard.h"
#include "touch_cal.h"

static fat32lite_fs_t sdfs;
static int sdfs_mounted;
static mutex_t sdfs_mutex = MUTEX_INIT;

void stm32_sdfs_lock(void) { mutex_lock(&sdfs_mutex); }
void stm32_sdfs_unlock(void) { mutex_unlock(&sdfs_mutex); }

static int sd_read(void *ctx, uint32_t lba, void *buf, uint32_t count) {
    (void)ctx;
    return stm32_sdcard_read(lba, buf, count); /* 0 ok, -1 err */
}
static int sd_write(void *ctx, uint32_t lba, const void *buf, uint32_t count) {
    (void)ctx;
    return stm32_sdcard_write(lba, buf, count);
}

int stm32_sdfs_mount(void) {
    sdfs_mounted = 0;
    const stm32_sdcard_info_t *info = stm32_sdcard_info();
    if (!info || !info->present)
        return FAT32LITE_EIO;

    fat32lite_io_t io = {0, sd_read, sd_write};
    int r = fat32lite_mount(&sdfs, &io, info->partition_lba);
    if (r == FAT32LITE_OK)
        sdfs_mounted = 1;
    return r;
}

int stm32_sdfs_ready(void) { return sdfs_mounted; }

fat32lite_fs_t *stm32_sdfs(void) { return sdfs_mounted ? &sdfs : 0; }

int stm32_sdfs_read_file(const char *path, void *buf, uint32_t max_len) {
    if (!sdfs_mounted)
        return FAT32LITE_EINVAL;
    fat32lite_file_t f;
    int r = fat32lite_open(&sdfs, path, &f);
    if (r)
        return r;
    uint32_t total = 0;
    uint8_t *out = buf;
    while (total < max_len) {
        int n = fat32lite_read(&f, out + total, max_len - total);
        if (n < 0) {
            fat32lite_close(&f);
            return n;
        }
        if (n == 0)
            break;
        total += (uint32_t)n;
    }
    fat32lite_close(&f);
    return (int)total;
}

int stm32_sdfs_write_file(const char *path, const void *buf, uint32_t len) {
    if (!sdfs_mounted)
        return FAT32LITE_EINVAL;
    fat32lite_file_t f;
    int r = fat32lite_create(&sdfs, path, &f);
    if (r)
        return r;
    const uint8_t *in = buf;
    uint32_t total = 0;
    while (total < len) {
        int n = fat32lite_write(&f, in + total, len - total);
        if (n < 0) {
            fat32lite_close(&f);
            return n;
        }
        total += (uint32_t)n;
    }
    r = fat32lite_close(&f);
    return r ? r : (int)total;
}

int stm32_sdfs_load_touch_cal(void) {
    uint8_t blob[TOUCH_CAL_BLOB_SIZE];
    int n = stm32_sdfs_read_file("/CFG/TOUCH.CAL", blob, sizeof(blob));
    if (n != (int)TOUCH_CAL_BLOB_SIZE)
        return n < 0 ? n : FAT32LITE_EINVAL;
    stm32_touch_calibration_t cal;
    if (touch_cal_deserialize(blob, sizeof(blob), &cal) != 0)
        return FAT32LITE_EINVAL;
    stm32_touch_set_calibration(&cal);
    return FAT32LITE_OK;
}

int stm32_sdfs_log(uint32_t ts_ms, const char *tag, const char *msg) {
    if (!sdfs_mounted)
        return FAT32LITE_EINVAL;
    char line[128];
    int n = hub_log_format(line, sizeof(line), ts_ms, tag, msg);
    if (n < 0)
        return FAT32LITE_EINVAL;
    fat32lite_mkdir(&sdfs, "/LOG"); /* best-effort; ignore EEXIST */
    fat32lite_file_t f;
    int r = fat32lite_append(&sdfs, "/LOG/RUN.LOG", &f);
    if (r != FAT32LITE_OK)
        return r;
    uint32_t total = 0;
    while (total < (uint32_t)n) {
        int w = fat32lite_write(&f, line + total, (uint32_t)n - total);
        if (w < 0) {
            fat32lite_close(&f);
            return w;
        }
        total += (uint32_t)w;
    }
    return fat32lite_close(&f);
}

int stm32_sdfs_load_config(hub_cfg_t *out) {
    if (!out)
        return FAT32LITE_EINVAL;
    char text[512];
    int n = stm32_sdfs_read_file("/CFG/WIFI.TXT", text, sizeof(text) - 1U);
    if (n < 0)
        return n;
    return hub_cfg_parse(text, (unsigned)n, out);
}

int stm32_sdfs_save_touch_cal(const stm32_touch_calibration_t *cal) {
    if (!cal)
        return FAT32LITE_EINVAL;
    uint8_t blob[TOUCH_CAL_BLOB_SIZE];
    if (touch_cal_serialize(cal, blob, sizeof(blob)) != (int)TOUCH_CAL_BLOB_SIZE)
        return FAT32LITE_EINVAL;
    /* best-effort: ensure /CFG exists (ignore EEXIST) */
    fat32lite_mkdir(&sdfs, "/CFG");
    return stm32_sdfs_write_file("/CFG/TOUCH.CAL", blob, sizeof(blob));
}

#endif /* CONFIG_BOARD_STM32F103 */
