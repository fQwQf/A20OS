/*
 * A20OS Native ABI — Error codes.
 * Design reference: docs/native-abi/02-errors.md
 */
#ifndef _ABI_NATIVE_ERRNO_H
#define _ABI_NATIVE_ERRNO_H

#define A20_OK                       0
#define A20_ERR_PERM                 1
#define A20_ERR_NO_ENTRY             2
#define A20_ERR_INTERRUPTED          3
#define A20_ERR_IO                   4
#define A20_ERR_BAD_HANDLE           5
#define A20_ERR_NO_MEMORY            6
#define A20_ERR_ACCESS               7
#define A20_ERR_FAULT                8
#define A20_ERR_BUSY                 9
#define A20_ERR_EXISTS               10
#define A20_ERR_NOT_SUPPORTED        11
#define A20_ERR_INVALID_ARGUMENT     12
#define A20_ERR_NO_SPACE             13
#define A20_ERR_NOT_DIR              14
#define A20_ERR_IS_DIR               15
#define A20_ERR_NOT_EMPTY            16
#define A20_ERR_NAME_TOO_LONG        17
#define A20_ERR_WOULD_BLOCK          18
#define A20_ERR_TIMED_OUT            19
#define A20_ERR_CANCELED             20
#define A20_ERR_PROTOCOL             21
#define A20_ERR_NOT_FOUND            24
#define A20_ERR_RANGE                22
#define A20_ERR_TYPE_MISMATCH        23
#define A20_ERR_EXPIRED              25

#endif
