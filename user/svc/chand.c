/*
 * chand — consumer service for the channel transport benchmark.
 *
 * Receives a 4-byte total on its endpoint (A20_CHAND_EP_SLOT), then
 * receives and verifies the same increasing byte pattern used by the
 * shmring benchmark.  Exit code 0 = data intact.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../svc/shmring_proto.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_handle_t ep = A20_CHAND_EP_HANDLE;

    uint8_t hdr[4];
    uint32_t hlen = sizeof(hdr);
    uint32_t hcnt = 0;
    if (a20_channel_recv(ep, hdr, &hlen, 0, &hcnt) < 0 || hlen != 4)
        return 2;
    uint32_t total = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                     ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);

    uint8_t buf[16384];
    uint32_t expect = 0;
    uint32_t got = 0;
    while (got < total) {
        uint32_t blen = sizeof(buf);
        hcnt = 0;
        a20_status_t st = a20_channel_recv(ep, buf, &blen, 0, &hcnt);
        if (st < 0)
            return 3;
        if (blen == 0 || got + blen > total)
            return 4;
        for (uint32_t i = 0; i < blen; i++) {
            if (buf[i] != (uint8_t)(expect & 0xff))
                return 1;
            expect++;
        }
        got += blen;
    }
    return 0;
}
