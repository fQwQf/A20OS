#ifndef _STARIVE_GMAC_H
#define _STARIVE_GMAC_H

#include "core/types.h"

#define STARFIVE_GMAC_MAC_BASE  0x0000
#define STARFIVE_GMAC_MTL_BASE  0x0C00
#define STARFIVE_GMAC_DMA_BASE  0x1000

/* Platform identity contract: the VisionFive2 board registers its GMAC as a
 * platform device with these IDs; the driver binds through platform_bus. */
#define STARFIVE_GMAC_PLATFORM_VENDOR 0x5F56U
#define STARFIVE_GMAC_PLATFORM_DEVICE 2U

int  starfive_gmac_init(uintptr_t base);
int  starfive_gmac_send(uintptr_t base, const void *pkt, size_t len);
int  starfive_gmac_recv(uintptr_t base, void *buf, size_t maxlen);
void starfive_gmac_get_mac(uintptr_t base, uint8_t *mac);
int  starfive_gmac_poll(uintptr_t base);

#endif
