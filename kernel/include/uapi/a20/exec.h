#ifndef A20_UAPI_EXEC_H
#define A20_UAPI_EXEC_H

#include <stdint.h>
#define A20_EXEC_STRUCT 0x4132304558454301ULL

/* Explicit A20_EXEC_STRUCT tag plus versioned request. Legacy execve keeps
 * ignoring unused registers. Bytes are delivered through the
 * E-APPEND startup descriptor. Handles are closed only at exec commit. */
#define A20_EXEC_BOOTSTRAP_MAX 65536u
#define A20_EXEC_CLOSE_MAX 4096u
typedef struct a20_exec_bootstrap_args {
    uint32_t size;
    uint32_t version;
    uint64_t path, argv, envp;
    uint64_t data;
    uint64_t data_size;
    uint64_t close_handles;
    uint32_t close_count;
    uint32_t reserved;
} a20_exec_bootstrap_args_t;

#endif
