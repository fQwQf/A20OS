#ifndef _STM32F103_SDFS_H
#define _STM32F103_SDFS_H

/*
 * TF-card filesystem: mounts the FAT32lite layer (kernel/fs/fat32lite.c) on top of the SDIO
 * sector driver (sdcard.c), using the partition LBA sdcard already parsed.
 * This is where the hub actually reads assets (backgrounds, Live2D frames,
 * fonts) and writes config/logs, since onboard Flash is too small for them.
 *
 * Thin hardware wiring: the FAT32 logic itself is covered by the host test in
 * tools/fat32-test (real image, cross-checked with mtools); here we just bind
 * it to stm32_sdcard_read/write.
 */

#include "core/types.h"
#include "fs/fat32lite.h"
#include "hub_cfg.h"
#include "drivers/stm32f1/touch.h"

/* Mount the card's FAT32 volume. Returns FAT32LITE_OK, or a negative FAT32LITE_E* /
 * FAT32LITE_EIO when the card is absent or unreadable. Safe to call again. */
int stm32_sdfs_mount(void);

int stm32_sdfs_ready(void);

/* Serialize FAT32lite and SDIO state across the storage, UI calibration and
 * diagnostic-console threads. The filesystem object is not re-entrant. */
void stm32_sdfs_lock(void);
void stm32_sdfs_unlock(void);

/* The mounted filesystem, or NULL if not mounted. */
fat32lite_fs_t *stm32_sdfs(void);

/* Convenience whole-file helpers (mount first). read returns bytes read or a
 * negative FAT32LITE_E*; write creates/truncates then writes `len` bytes. */
int stm32_sdfs_read_file(const char *path, void *buf, uint32_t max_len);
int stm32_sdfs_write_file(const char *path, const void *buf, uint32_t len);

/* Touch calibration persistence at /cfg/touch.cal (see touch_cal.{c,h}).
 * load: read + deserialize + apply via stm32_touch_set_calibration().
 * save: serialize the current calibration, creating /cfg if needed. */
int stm32_sdfs_load_touch_cal(void);
int stm32_sdfs_save_touch_cal(const stm32_touch_calibration_t *cal);

/* Read + parse /CFG/WIFI.TXT into out. Returns the recognised-key count (>=0),
 * or a negative FAT32LITE_E* when the file is missing/unreadable. */
int stm32_sdfs_load_config(hub_cfg_t *out);

/* Append a timestamped line to /LOG/RUN.LOG (creating /LOG on first use).
 * ts_ms is uptime in ms. Returns FAT32LITE_OK or a negative FAT32LITE_E*. */
int stm32_sdfs_log(uint32_t ts_ms, const char *tag, const char *msg);

#endif /* _STM32F103_SDFS_H */
