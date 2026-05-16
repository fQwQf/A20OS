#ifndef _ABI_CURRENT_H
#define _ABI_CURRENT_H

/*
 * Linux ABI constants are always included — they are used by kernel-internal
 * code (VFS flags, trap signals, errno) regardless of which userspace ABI is
 * active.  The native ABI headers below define the *userspace* syscall
 * interface, not the kernel's internal representation.
 */
#include "abi/linux/errno.h"
#include "abi/linux/fcntl.h"
#include "abi/linux/mman.h"
#include "abi/linux/poll.h"
#include "abi/linux/signal.h"
#include "abi/linux/stat.h"

#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
# include "abi/linux/futex.h"
# include "abi/linux/ioctl.h"
# include "abi/linux/mman.h"
# include "abi/linux/poll.h"
# include "abi/linux/resource.h"
#endif

#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
# include "abi/native/types.h"
# include "abi/native/errno.h"
# include "abi/native/rights.h"
# include "abi/native/syscall_nr.h"
# include "abi/native/startup.h"
#endif

#if !defined(CONFIG_ABI_LINUX) && !defined(CONFIG_ABI_NATIVE) && !defined(CONFIG_ABI_BOTH)
# error "No user ABI selected"
#endif

#endif
