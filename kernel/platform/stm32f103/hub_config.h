/*
 * Smart home hub — cloud-proxy address (build-time default).
 *
 * This is only the *fallback* proxy address the firmware boots with. Two things
 * override it at runtime, and either is preferable to editing this file:
 *   - /CFG/WIFI.TXT on the TF card  (hub_cfg.h) — applied at boot
 *   - the `proxy <ip> <port>` console command   — applied immediately
 *
 * WiFi credentials are deliberately NOT here. They come from, in order:
 *   1. /CFG/WIFI.TXT on the card  -> one-shot join at boot (preferred: keeps
 *      credentials out of the firmware image, and survives a rebuild)
 *   2. `make ... STM32_WIFI_SSID=x STM32_WIFI_PASSWORD=y` -> baked into wifi.c
 *      via a generated header; wifi.c auto-joins after AT bring-up when the
 *      SSID is non-empty
 *   3. the `wifi join <ssid> <password>` console command
 * With none of them set the module comes up READY but unjoined, which is the
 * intended default for a firmware image that might be flashed anywhere.
 *
 * (There used to be HUB_WIFI_SSID/HUB_WIFI_PASS macros here. Nothing ever read
 * them — setting them looked like it configured WiFi and silently did nothing.)
 */
#ifndef _STM32F103_HUB_CONFIG_H
#define _STM32F103_HUB_CONFIG_H

#define HUB_PROXY_IP "192.168.1.100"
#define HUB_PROXY_PORT 9000

#endif /* _STM32F103_HUB_CONFIG_H */
