#ifndef _ABI_SYSCALL_ENTRY_H
#define _ABI_SYSCALL_ENTRY_H

#if defined(CONFIG_ABI_LINUX)
# include "abi/linux/syscall_entry.h"
#else
# error "No syscall ABI selected"
#endif

#endif /* _ABI_SYSCALL_ENTRY_H */
