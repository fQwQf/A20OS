#ifndef _DRIVERS_CHAR_CMOS_RTC_H
#define _DRIVERS_CHAR_CMOS_RTC_H

#include "drivers/audio/pc_speaker.h" /* A20_PLATFORM_VENDOR */

/*
 * MC146818 (PC CMOS/RTC) identity and register map, shared by the board files
 * that declare the platform device and by the cmos-rtc driver that reads it.
 *
 * A20_PLATFORM_VENDOR/A20_DEVICE_* live in pc_speaker.h; this header only adds
 * the CMOS identity next to them so both board platforms and the module agree
 * on one number.
 */
#define A20_DEVICE_CMOS_RTC 0x00000002U

/*
 * Index (port 0x70) / data (port 0x71) register numbers.  This is the numbering
 * the firmware and emulator agree on. SeaBIOS's table in the external
 * Debian QEMU 10.0.13+ds tree (roms/seabios-hppa/src/hw/rtc.h:11-25,36)
 * places time registers at 0x00/0x02/0x04/0x06/0x07/0x08/0x09 and A..D at
 * 0x0a..0x0d and the century byte at 0x32; Linux (include/linux/mc146818rtc.h)
 * and QEMU's mc146818 model use the same indices.  So register A does *not*
 * alias the month, and the date can be read without decoding away mode bits.
 * Source provenance: docs/history/2026-10-08/migration.md.
 */
#define CMOS_RTC_SEC          0x00
#define CMOS_RTC_MIN          0x02
#define CMOS_RTC_HOUR         0x04
#define CMOS_RTC_WDAY         0x06
#define CMOS_RTC_MDAY         0x07
#define CMOS_RTC_MONTH        0x08
#define CMOS_RTC_YEAR         0x09
#define CMOS_RTC_REG_A        0x0a
#define CMOS_RTC_REG_B        0x0b
#define CMOS_RTC_REG_C        0x0c
#define CMOS_RTC_REG_D        0x0d
/*
 * Not register C or D (those are the read-to-clear interrupt flags and RAM
 * validity).  0x32 is the century byte of the PC-compatible CMOS RAM, which is
 * where the BIOS writes the century and where Linux reads it on x86; IBM PS/2
 * aliases it at 0x37.  A machine that never programmed it reads 0x00 or 0xff.
 */
#define CMOS_RTC_CENTURY      0x32

#define CMOS_RTC_REGA_UIP     0x80 /* update in progress: registers are unstable */
/* Register A holds the divider/rate select and UIP only; the data encoding and
 * the hour format both live in register B (RTC_B_BIN/RTC_B_24HR). */
#define CMOS_RTC_REGB_SET     0x80 /* update inhibit; 1 stops the clock */
#define CMOS_RTC_REGB_BINARY  0x04 /* 1 = binary counters, 0 = BCD */
#define CMOS_RTC_REGB_24HOUR  0x02 /* 1 = 24-hour clock, 0 = 12-hour + PM flag */
#define CMOS_RTC_HOUR_PM      0x80 /* 12-hour mode PM flag */

#endif /* _DRIVERS_CHAR_CMOS_RTC_H */