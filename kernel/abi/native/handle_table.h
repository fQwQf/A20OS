/*
 * A20OS Native ABI — handle table re-export.
 * The handle table is internal (kernel/include/ipc/handle_table.h).
 *
 * NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX: handle rights are validated
 * against the object-type right mask at install/dup/transfer; temporal
 * limits and security labels travel with each handle entry.  See
 * kernel/include/ipc/handle_table.h and docs/native-abi/06-security.md.
 */
#ifndef _ABI_NATIVE_HANDLE_TABLE_INTERNAL_H
#define _ABI_NATIVE_HANDLE_TABLE_INTERNAL_H

#include "ipc/handle_table.h"

#endif
