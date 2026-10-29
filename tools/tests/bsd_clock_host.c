/* Link the real BSD clock adapter with a controlled A20OS clock source.
 * Verify clock-domain separation, long uptime and optional result fields. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "ff_host_interface.h"

static uint64_t mono[2], real[2];
void timekeeping_get_monotonic(uint64_t out[2])
{
    out[0] = mono[0]; out[1] = mono[1];
}
void timekeeping_get_realtime(uint64_t out[2])
{
    out[0] = real[0]; out[1] = real[1];
}
int main(void)
{
    int64_t sec;
    long nsec;
    mono[0] = 200 * 86400; mono[1] = 987654321;
    real[0] = 1793232000; real[1] = 123456789;
    ff_clock_gettime(ff_CLOCK_MONOTONIC, &sec, &nsec);
    assert(sec == 17280000 && nsec == 987654321);
    assert(ff_get_tsc_ns() == UINT64_C(17280000987654321));
    assert(ff_clock_gettime_ns(ff_CLOCK_MONOTONIC_FAST) == ff_get_tsc_ns());
    assert(ff_clock_gettime_ns(999) == ff_get_tsc_ns());
    ff_clock_gettime(ff_CLOCK_REALTIME, &sec, &nsec);
    assert(sec == 1793232000 && nsec == 123456789);
    ff_get_current_time(&sec, NULL);
    assert(sec == 1793232000);
    ff_get_current_time(NULL, &nsec);
    assert(nsec == 123456789);
    ff_get_current_time(NULL, NULL);
    /* Simulate a backwards wall-clock correction during TCP retransmission. */
    real[0] -= 3600;
    mono[1] += 100;
    assert(ff_get_tsc_ns() == UINT64_C(17280000987654421));
    assert(ff_clock_gettime_ns(ff_CLOCK_REALTIME) == UINT64_C(1793228400123456789));
    /* A second boundary and a fresh realtime read require no poll refresh. */
    mono[0]++; mono[1] = 5;
    assert(ff_get_tsc_ns() == UINT64_C(17280001000000005));
    real[1] = 765432100;
    ff_get_current_time(&sec, &nsec);
    assert(sec == 1793228400 && nsec == 765432100);
    puts("BSD_CLOCK_HOST_PASS");
    return 0;
}
