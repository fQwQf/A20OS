#ifndef _IPC_TIMERFD_H
#define _IPC_TIMERFD_H

#include "core/types.h"

int timerfd_create_file(int clockid, int flags);
int timerfd_settime_file(int gfd, int flags, const uint64_t new_value[4], uint64_t old_value[4]);
int timerfd_gettime_file(int gfd, uint64_t curr_value[4]);

#endif /* _IPC_TIMERFD_H */
