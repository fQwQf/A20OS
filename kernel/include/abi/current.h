#ifndef _ABI_CURRENT_H
#define _ABI_CURRENT_H

#if defined(CONFIG_ABI_LINUX)
# include "abi/linux/errno.h"
# include "abi/linux/fcntl.h"
# include "abi/linux/futex.h"
# include "abi/linux/ioctl.h"
# include "abi/linux/mman.h"
# include "abi/linux/poll.h"
# include "abi/linux/resource.h"
# include "abi/linux/signal.h"
# include "abi/linux/stat.h"
#else
# error "No user ABI selected"
#endif

#endif /* _ABI_CURRENT_H */
