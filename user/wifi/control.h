#ifndef A20_WIFI_CONTROL_H
#define A20_WIFI_CONTROL_H
#include <stdint.h>
#define WIFI_SOCKET "/run/a20-wifi.sock"
#define WIFI_COMMAND_VERSION 1
#define WIFI_SCAN 1
#define WIFI_STATUS 2
#define WIFI_CONNECT 3
#define WIFI_DISCONNECT 4
#define WIFI_SCAN_JSON 5
struct wifi_command {
    uint32_t version, operation, ssid_length, password_length;
    char ssid[32], password[64];
};
struct wifi_reply {
    int32_t result;
    uint32_t length;
};
#endif
