#ifndef A20_CLUSTER_SSI_H
#define A20_CLUSTER_SSI_H

#include <stdint.h>

/* Experimental x86-64 Linux thread execution slices. All fields are host
 * endian; this ABI and its network protocol require identical kernel builds. */
#define A20_PR_CLUSTER_SSI 0xA210
#define A20_SSI_VERSION 1
#define A20_SSI_OFF 0
#define A20_SSI_HOME 1
#define A20_SSI_WORKER 2
#define A20_SSI_PAGE_LIMIT 64
#define A20_SSI_THREAD_LIMIT 16

struct a20_ssi_config {
    uint32_t version, role;
    uint32_t shared_memory, automatic_threads;
    uint32_t max_pages, max_threads, max_slices, quantum_ticks;
    uint32_t timeout_ms, max_bytes;
    uint8_t address[4]; /* peer for home, bind address for worker */
    uint16_t port, reserved;
    uint8_t token[32]; /* exact shared token, controlled network only */
};
_Static_assert(sizeof(struct a20_ssi_config) == 80, "SSI configuration ABI");
#endif
