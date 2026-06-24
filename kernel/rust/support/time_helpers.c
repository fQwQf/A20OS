#include <stdint.h>
#include <build_time.h>
#include <core/timekeeping.h>

uint64_t a20_build_unix_time(void)
{
    return A20_BUILD_UNIX_TIME;
}

void a20_timekeeping_get_realtime(uint64_t *now)
{
    timekeeping_get_realtime(now);
}
