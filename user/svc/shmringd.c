/*
 * shmringd — consumer service for the shared-VMO ring benchmark.
 *
 * Maps the VMO handed over at A20_SHMRING_VMO_SLOT, attaches to the ring,
 * signals ready, then consumes total bytes and verifies the increasing
 * byte pattern.  Exit code 0 = data intact, 1 = corruption detected.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_shmring.h"
#include "../svc/shmring_proto.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    uint64_t base = 0;
    if (a20_vm_map(A20_SHMRING_VMO_HANDLE, A20_SHMRING_VMO_SIZE, 0,
                   A20_PROT_READ | A20_PROT_WRITE, &base) < 0)
        return 2;
    a20_shmring_t *r = (a20_shmring_t *)(uintptr_t)base;
    if (a20_shmring_attach(r) < 0)
        return 3;

    uint32_t total = r->total_lo;
    a20_shmring_signal_ready(r);

    uint8_t buf[32768];
    uint32_t expect = 0; /* next pattern byte index (mod 256) */
    uint32_t got = 0;
    while (got < total) {
        uint32_t want = total - got;
        if (want > sizeof(buf)) want = sizeof(buf);
        uint32_t n = a20_shmring_read(r, buf, want);
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] != (uint8_t)(expect & 0xff))
                return 1;
            expect++;
        }
        got += n;
    }

    a20_shmring_signal_done(r);
    return 0;
}
