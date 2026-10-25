#ifdef CONFIG_BOARD_STM32F103

#include "sdcard_fs.h"
#include "core/string.h"
#include "drivers/stm32f1/sdcard.h"
#include "drivers/stm32f1/touch.h"
#include "drivers/stm32f1/touch_cal.h"

static fat32lite_fs_t fs;
static int mounted;

static int read_blocks(void *ctx, uint32_t lba, void *buf, uint32_t count) {
    (void)ctx;
    return stm32_sdcard_read(lba, buf, count);
}

static int write_blocks(void *ctx, uint32_t lba, const void *buf,
                        uint32_t count) {
    (void)ctx;
    return stm32_sdcard_write(lba, buf, count);
}

int stm32_sdcard_fs_mount(void) {
    const stm32_sdcard_info_t *info = stm32_sdcard_info();
    fat32lite_io_t io = {0, read_blocks, write_blocks};
    mounted = 0;
    if (!info || !info->present)
        return FAT32LITE_EIO;
    int r = fat32lite_mount(&fs, &io, info->partition_lba);
    if (r == FAT32LITE_OK)
        mounted = 1;
    return r;
}

void stm32_sdcard_fs_unmount(void) {
    mounted = 0;
    memset(&fs, 0, sizeof(fs));
}

fat32lite_fs_t *stm32_sdcard_fs(void) { return mounted ? &fs : 0; }

int stm32_sdcard_fs_load_touch_calibration(void) {
    uint8_t blob[TOUCH_CAL_BLOB_SIZE];
    fat32lite_file_t file;
    stm32_touch_calibration_t calibration;

    if (!mounted || fat32lite_open(&fs, "/CFG/TOUCH.CAL", &file) != 0)
        return -1;
    int length = fat32lite_read(&file, blob, sizeof(blob));
    (void)fat32lite_close(&file);
    if (length != (int)sizeof(blob) ||
        touch_cal_deserialize(blob, sizeof(blob), &calibration) != 0)
        return -1;
    stm32_touch_set_calibration(&calibration);
    return 0;
}

#endif
