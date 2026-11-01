/*
 * Minimal Hi3861 SDK base types.
 *
 * Only the names the textbook experiments use are provided.  This header is
 * vendor surface: it is not part of CMSIS-RTOS2 and no file under
 * user/cmsis_rtos2/ may include it.
 */
#ifndef HI_TYPES_BASE_H
#define HI_TYPES_BASE_H

typedef unsigned char      hi_u8;
typedef unsigned short     hi_u16;
typedef unsigned int       hi_u32;
typedef unsigned long long hi_u64;
typedef signed char        hi_s8;
typedef short              hi_s16;
typedef int                hi_s32;
typedef long long          hi_s64;
typedef void               hi_void;
typedef unsigned int       hi_size_t;

#define HI_ERR_SUCCESS 0U
#define HI_ERR_FAILURE 1U
#define HI_TRUE        1U
#define HI_FALSE       0U

#endif /* HI_TYPES_BASE_H */
