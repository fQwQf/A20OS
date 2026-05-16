/*
 * A20OS Native ABI — 14 capability rights bits.
 * Design reference: docs/native-abi/06-security.md §1
 */
#ifndef _ABI_NATIVE_RIGHTS_H
#define _ABI_NATIVE_RIGHTS_H

#include "types.h"

#define A20_RIGHT_READ       (1ull << 0)
#define A20_RIGHT_WRITE      (1ull << 1)
#define A20_RIGHT_EXEC       (1ull << 2)
#define A20_RIGHT_STAT       (1ull << 3)
#define A20_RIGHT_SEEK       (1ull << 4)
#define A20_RIGHT_DUP        (1ull << 5)
#define A20_RIGHT_TRANSFER   (1ull << 6)
#define A20_RIGHT_MAP        (1ull << 7)
#define A20_RIGHT_WAIT       (1ull << 8)
#define A20_RIGHT_CONNECT    (1ull << 9)
#define A20_RIGHT_ACCEPT     (1ull << 10)
#define A20_RIGHT_CONTROL    (1ull << 11)
#define A20_RIGHT_ADMIN      (1ull << 12)
#define A20_RIGHT_SIGNAL     (1ull << 13)

#define A20_RIGHTS_ALL  (A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_EXEC | \
                         A20_RIGHT_STAT | A20_RIGHT_SEEK | A20_RIGHT_DUP | \
                         A20_RIGHT_TRANSFER | A20_RIGHT_MAP | A20_RIGHT_WAIT | \
                         A20_RIGHT_CONNECT | A20_RIGHT_ACCEPT | A20_RIGHT_CONTROL | \
                         A20_RIGHT_ADMIN | A20_RIGHT_SIGNAL)

#define A20_RIGHTS_NONE ((a20_rights_t)0)

#endif
