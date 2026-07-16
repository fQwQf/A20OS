/*
 * Smart home hub — network configuration.
 *
 * PLACEHOLDERS — edit before flashing, or (better, TODO) load from the TF card
 * at /cfg/wifi.txt so credentials aren't baked into the firmware image.
 * HUB_PROXY_IP/PORT point at the machine running tools/hub-proxy.
 */
#ifndef _STM32F103_HUB_CONFIG_H
#define _STM32F103_HUB_CONFIG_H

#define HUB_WIFI_SSID "your-ssid"
#define HUB_WIFI_PASS "your-password"
#define HUB_PROXY_IP "192.168.1.100"
#define HUB_PROXY_PORT 9000

#endif /* _STM32F103_HUB_CONFIG_H */
