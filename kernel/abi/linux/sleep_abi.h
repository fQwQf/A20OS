#ifndef A20_LINUX_SLEEP_ABI_H
#define A20_LINUX_SLEEP_ABI_H

/* Explicit Linux wire layouts, independent of the kernel C long width. */
static inline int linux_sleep_read_timespec(const void *user, int time32,
                                            uint64_t out[2]) {
    int64_t sec, nsec;
    if (time32) {
        int32_t wire[2];
        if (copy_from_user(wire, user, sizeof(wire)) < 0)
            return -EFAULT;
        sec = wire[0];
        nsec = wire[1];
    } else {
        int64_t wire[2];
        if (copy_from_user(wire, user, sizeof(wire)) < 0)
            return -EFAULT;
        sec = wire[0];
        nsec = wire[1];
    }
    if (sec < 0 || nsec < 0 || nsec >= 1000000000LL)
        return -EINVAL;
    out[0] = (uint64_t)sec;
    out[1] = (uint64_t)nsec;
    return 0;
}

static inline int linux_sleep_write_timespec(void *user, int time32,
                                             const uint64_t value[2]) {
    if (time32) {
        if (value[0] > 0x7fffffffULL)
            return -EOVERFLOW;
        int32_t wire[2] = {(int32_t)value[0], (int32_t)value[1]};
        return copy_to_user(user, wire, sizeof(wire)) < 0 ? -EFAULT : 0;
    }
    return copy_to_user(user, value, sizeof(uint64_t) * 2) < 0 ? -EFAULT : 0;
}

/* A representable positive time64 request must never wrap into a past tick.
 * Saturate at the last deadline representable by this timer implementation. */
static inline uint64_t linux_sleep_deadline(uint64_t sec, uint64_t nsec,
                                            uint64_t now) {
    uint64_t fraction = (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
    uint64_t limit = UINT64_MAX - now;
    if (fraction > limit || sec > (limit - fraction) / TICKS_PER_SEC)
        return UINT64_MAX;
    return now + sec * TICKS_PER_SEC + fraction;
}

#endif
