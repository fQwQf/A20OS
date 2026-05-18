#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clockid_t;
typedef long suseconds_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

int    clock_gettime(clockid_t clk, struct timespec *ts);
time_t time(time_t *t);

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1

#endif
