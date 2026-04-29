#ifndef _IPC_EVENTFD_H
#define _IPC_EVENTFD_H

#include "core/types.h"

int eventfd_create(unsigned initval, int flags);

#endif /* _IPC_EVENTFD_H */
