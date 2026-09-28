/*
 * shmringd — consumer service for the shared-VMO ring benchmark.
 *
 * Maps the VMO handed over at A20_SHMRING_VMO_SLOT, attaches to the ring,
 * signals ready, then consumes total bytes and verifies the increasing
 * byte pattern.  Exit code 0 = data intact, 1 = corruption detected.
 */
#include "liba20rt/a20_sdk.h"
#include "a20_services_idl.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_shmring.h"

static a20_handle_t g_out = A20_HANDLE_NULL;

static void say(const char *s, uint32_t len)
{
    if (g_out != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_out, s, len, (void *)0);
}

static void say_str(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    say(s, n);
}

static int bail(int code, const char *why)
{
    say_str("SHMRINGD: FAIL ");
    say_str(why);
    say("\n", 1);
    return code;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    say_str("SHMRINGD: entered main\n");

    uint64_t base = 0;
    if (a20_vm_map(((a20_handle_t)A20_SHMRING_VMO_SLOT), A20_SHMRING_VMO_SIZE, 0,
                   A20_PROT_READ | A20_PROT_WRITE, &base) < 0)
        return bail(A20_SHMRING_EXIT_VM_MAP, "vm_map(slot) failed");
    a20_shmring_t *r = (a20_shmring_t *)(uintptr_t)base;
    if (a20_shmring_attach(r) < 0)
        return bail(A20_SHMRING_EXIT_ATTACH, "attach failed (magic mismatch)");

    uint32_t total = r->total_lo;
    a20_shmring_signal_ready(r);

    uint8_t buf[32768];
    uint32_t expect = 0; /* next pattern byte index (mod 256) */
    uint32_t got = 0;
    while (got < total) {
        uint32_t want = total - got;
        if (want > sizeof(buf)) want = sizeof(buf);
        uint32_t n = a20_shmring_read(r, buf, want);
        if (n == 0)
            return bail(A20_SHMRING_EXIT_SHORT_READ, "read returned 0 before total");
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] != (uint8_t)(expect & 0xff))
                return bail(A20_SHMRING_EXIT_CORRUPT, "data corruption");
            expect++;
        }
        got += n;
    }

    a20_shmring_signal_done(r);
    return 0;
}
