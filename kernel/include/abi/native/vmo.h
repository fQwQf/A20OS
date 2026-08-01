/*
 * A20OS Native ABI — VMO (Virtual Memory Object) compatibility header.
 *
 * The VMO type and implementation live in the core MM layer (mm/vmo.h,
 * mm/vmo.c).  This header only keeps the Native ABI name alias so ABI code
 * can refer to a20_vmo_t without reimplementing any memory management.
 */
#ifndef _ABI_NATIVE_VMO_H
#define _ABI_NATIVE_VMO_H

#include "mm/vmo.h"

typedef struct vmo a20_vmo_t;

#endif /* _ABI_NATIVE_VMO_H */
