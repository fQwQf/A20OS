#ifndef _LS2K_GMAC_H
#define _LS2K_GMAC_H

#include "core/types.h"

int  ls2k_gmac_init(uintptr_t base);
int  ls2k_gmac_send(uintptr_t base, const void *pkt, size_t len);
int  ls2k_gmac_recv(uintptr_t base, void *buf, size_t maxlen);
void ls2k_gmac_get_mac(uintptr_t base, uint8_t *mac);
int  ls2k_gmac_poll(uintptr_t base);

#endif
