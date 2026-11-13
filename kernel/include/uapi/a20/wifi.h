#ifndef A20_UAPI_WIFI_H
#define A20_UAPI_WIFI_H

/* Root-only hardware transport for the independently licensed rtw89 service.
 * Fixed-width fields, no userspace pointers or kernel addresses. */
typedef __UINT32_TYPE__ a20_wifi_u32;
typedef __UINT64_TYPE__ a20_wifi_u64;
typedef __UINT16_TYPE__ a20_wifi_u16;
typedef __UINT8_TYPE__ a20_wifi_u8;

#define A20_WIFI_ABI 1U
#define A20_WIFI_INFO       0x5701U
#define A20_WIFI_CFG_READ   0x5702U
#define A20_WIFI_CFG_WRITE  0x5703U
#define A20_WIFI_DMA_ALLOC  0x5704U
#define A20_WIFI_DMA_FREE   0x5705U
#define A20_WIFI_LINK       0x5706U
#define A20_WIFI_QUIESCE    0x5707U
#define A20_WIFI_HWADDR     0x5708U
#define A20_WIFI_DMA_OFFSET (8ULL << 20)

struct a20_wifi_info {
    a20_wifi_u32 version;
    a20_wifi_u16 vendor, device, subvendor, subdevice;
    a20_wifi_u8 revision, mac[6], reserved;
    a20_wifi_u64 mmio_size;
};
struct a20_wifi_cfg {
    a20_wifi_u32 offset, width, value, reserved;
};
struct a20_wifi_dma {
    a20_wifi_u64 size, token, bus_address;
};
struct a20_wifi_link {
    a20_wifi_u32 authorized, reserved;
};
struct a20_wifi_hwaddr {
    a20_wifi_u8 mac[6], reserved[2];
};
#endif
