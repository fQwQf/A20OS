/* SPDX-License-Identifier: BSD-3-Clause */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char a20_rtw89_firmware_begin[], a20_rtw89_firmware_end[];

struct firmware {
    size_t size;
    const uint8_t *data;
    size_t allocated;
};

int rtw88_load_firmware_sync(const char *name, const struct firmware **out)
{
    if (!name || !out || strcmp(name, "rtw89/rtw8852b_fw-2.bin"))
        return -2;
    struct firmware *firmware = malloc(sizeof(*firmware));
    if (!firmware)
        return -12;
    *firmware = (struct firmware){
        .size = (size_t)(a20_rtw89_firmware_end - a20_rtw89_firmware_begin),
        .data = a20_rtw89_firmware_begin,
    };
    *out = firmware;
    return 0;
}

int request_firmware_nowait(void *module, int event, const char *name,
                            void *device, unsigned flags, void *context,
                            void (*complete)(const struct firmware *, void *))
{
    (void)module;
    (void)event;
    (void)device;
    (void)flags;
    const struct firmware *firmware = NULL;
    if (!complete)
        return -22;
    (void)rtw88_load_firmware_sync(name, &firmware);
    complete(firmware, context);
    return 0;
}

void release_firmware(const struct firmware *firmware) { free((void *)firmware); }
