#ifndef A20_UAPI_THREAD_EXIT_H
#define A20_UAPI_THREAD_EXIT_H
#include <stdint.h>
/* Scalar exit(code) keeps ignoring every other argument. The full-width tag
 * cannot be a sign-extended int32 exit code and selects the versioned form. */
#define A20_EXIT_STRUCT 0x4132304558495401ULL
#define A20_THREAD_EXIT_DETACHED 1u
#define A20_THREAD_CREATE_JOINABLE 1u
#define A20_EXIT_CAUSE_NORMAL 0u
#define A20_EXIT_CAUSE_NOTIFICATION 1u

typedef struct a20_exit_args {
    uint32_t size, version;
    int32_t code;
    uint32_t cause, notification, flags;
    uint64_t release_base, release_length;
} a20_exit_args_t;
#endif
