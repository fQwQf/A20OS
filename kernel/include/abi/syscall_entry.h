#ifndef _ABI_SYSCALL_ENTRY_H
#define _ABI_SYSCALL_ENTRY_H

#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
# include "abi/linux/syscall_entry.h"
#endif

#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
# include "abi/native/syscall_entry.h"
#endif

#if !defined(CONFIG_ABI_LINUX) && !defined(CONFIG_ABI_NATIVE) && !defined(CONFIG_ABI_BOTH)
# error "No syscall ABI selected"
#endif

#endif
