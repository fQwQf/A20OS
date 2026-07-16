#ifndef _STM32F103_HUB_CFG_H
#define _STM32F103_HUB_CFG_H

/*
 * Hub runtime configuration parsed from the TF card (/CFG/WIFI.TXT), so WiFi
 * credentials and the proxy address aren't baked into the firmware image.
 *
 * Text format (one key=value per line; '#' comments and blank lines ignored;
 * keys case-insensitive; surrounding spaces and optional quotes trimmed):
 *
 *   ssid  = MyNetwork
 *   pass  = secret123
 *   proxy = 192.168.1.100:9000
 *
 * Pure text parsing — covered by the host unit test.
 */

#include "core/types.h"

#define HUB_CFG_SSID_MAX 32U
#define HUB_CFG_PASS_MAX 64U
#define HUB_CFG_IP_MAX 15U

typedef struct hub_cfg {
    char ssid[HUB_CFG_SSID_MAX + 1U];
    char pass[HUB_CFG_PASS_MAX + 1U];
    char proxy_ip[HUB_CFG_IP_MAX + 1U];
    uint16_t proxy_port;
    uint8_t have_ssid;
    uint8_t have_pass;
    uint8_t have_proxy;
} hub_cfg_t;

/*
 * Parse `len` bytes of config text into out (zeroed first). The buffer need not
 * be NUL-terminated. Returns the number of recognised keys (>=0), or -1 on bad
 * args. Unknown keys and malformed proxy values are skipped, not fatal.
 */
int hub_cfg_parse(const char *text, unsigned len, hub_cfg_t *out);

#endif /* _STM32F103_HUB_CFG_H */
