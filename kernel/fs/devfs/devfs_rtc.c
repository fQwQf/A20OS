/*
 * devfs RTC device backend: /dev/rtc, /dev/rtc0.
 *
 * Provides the Linux rtc ioctl subset (RTC_RD_TIME, RTC_SET_TIME,
 * RTC_IRQP_READ, RTC_EPOCH_READ) over the kernel realtime clock.  Split out
 * of devfs.c to keep the node-table core small.
 */

#include "devfs_internal.h"

#include "core/errno.h"
#include "core/string.h"
#include "core/timekeeping.h"
#include "sys/usercopy.h"

#define RTC_RD_TIME    0x80247009UL
#define RTC_SET_TIME   0x4024700aUL
#define RTC_IRQP_READ  0x8008700bUL
#define RTC_EPOCH_READ 0x8008700dUL

typedef struct {
    int32_t tm_sec;
    int32_t tm_min;
    int32_t tm_hour;
    int32_t tm_mday;
    int32_t tm_mon;
    int32_t tm_year;
    int32_t tm_wday;
    int32_t tm_yday;
    int32_t tm_isdst;
} krtc_time_t;

static int rtc_is_leap_year(int year) {
    return (year % 4 == 0) && ((year % 100) != 0 || (year % 400) == 0);
}

static int rtc_time_to_unix_seconds(const krtc_time_t *rt, uint64_t *secs) {
    static const int month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int year = rt->tm_year + 1900;
    int month = rt->tm_mon + 1;
    int hour = rt->tm_hour;
    int min = rt->tm_min;
    int sec = rt->tm_sec;
    int mday = rt->tm_mday;
    int mdays;
    int64_t z;
    int64_t era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;

    if (year < 1970 || month < 1 || month > 12 || mday < 1 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
        return -EINVAL;

    mdays = month_days[month - 1];
    if (month == 2 && rtc_is_leap_year(year)) mdays = 29;
    if (mday > mdays) return -EINVAL;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    yoe = (unsigned)(year - era * 400);
    doy = (153U * (unsigned)(month + (month > 2 ? -3 : 9)) + 2U) / 5U + (unsigned)mday - 1U;
    doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    z = era * 146097 + (int64_t)doe - 719468;
    if (z < 0) return -EINVAL;

    *secs = (uint64_t)z * 86400ULL + (uint64_t)hour * 3600ULL +
            (uint64_t)min * 60ULL + (uint64_t)sec;
    return 0;
}

static void unix_seconds_to_rtc_time(uint64_t secs, krtc_time_t *rt) {
    static const int month_yday[2][12] = {
        { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 },
        { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 },
    };
    uint64_t days = secs / 86400ULL;
    uint64_t rem = secs % 86400ULL;
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = (int)(yoe + era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned mday = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp + (mp < 10 ? 3 : (unsigned)-9);

    year += (month <= 2);
    memset(rt, 0, sizeof(*rt));
    rt->tm_sec = (int32_t)(rem % 60ULL);
    rem /= 60ULL;
    rt->tm_min = (int32_t)(rem % 60ULL);
    rem /= 60ULL;
    rt->tm_hour = (int32_t)rem;
    rt->tm_mday = (int32_t)mday;
    rt->tm_mon = (int32_t)(month - 1);
    rt->tm_year = (int32_t)(year - 1900);
    rt->tm_wday = (int32_t)((days + 4ULL) % 7ULL);
    rt->tm_yday = month_yday[rtc_is_leap_year(year)][month - 1] + (int)mday - 1;
    rt->tm_isdst = 0;
}

int devfs_rtc_ioctl(unsigned long req, void *arg) {
    if ((req == RTC_RD_TIME || req == RTC_IRQP_READ || req == RTC_EPOCH_READ) && !arg)
        return -EFAULT;

    switch (req) {
    case RTC_RD_TIME: {
        krtc_time_t tm;
        uint64_t ts[2];
        timekeeping_get_realtime(ts);
        unix_seconds_to_rtc_time(ts[0], &tm);
        if (copy_to_user(arg, &tm, sizeof(tm)) < 0) return -EFAULT;
        return 0;
    }
    case RTC_IRQP_READ: {
        unsigned long irqp = 1;
        if (copy_to_user(arg, &irqp, sizeof(irqp)) < 0) return -EFAULT;
        return 0;
    }
    case RTC_EPOCH_READ: {
        unsigned long epoch = 1900;
        if (copy_to_user(arg, &epoch, sizeof(epoch)) < 0) return -EFAULT;
        return 0;
    }
    case RTC_SET_TIME: {
        krtc_time_t tm;
        uint64_t secs;
        if (copy_from_user(&tm, arg, sizeof(tm)) < 0) return -EFAULT;
        if (rtc_time_to_unix_seconds(&tm, &secs) < 0) return -EINVAL;
        return timekeeping_set_realtime(secs, 0);
    }
    default:
        return -ENOTTY;
    }
}

/* RTC has no read/write payload; reuse the devfs null semantics. */
static int devfs_rtc_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf; (void)buf; (void)count;
    return 0;
}
static int devfs_rtc_write(vfile_t *vf, const char *buf, size_t count) {
    (void)vf; (void)buf;
    return (int)count;
}

static int devfs_rtc_ops_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    (void)vf;
    return devfs_rtc_ioctl(req, arg);
}

vfile_ops_t g_devfs_rtc_ops =
    { .read = devfs_rtc_read, .write = devfs_rtc_write,
      .lseek = devfs_noop_lseek, .ioctl = devfs_rtc_ops_ioctl };
