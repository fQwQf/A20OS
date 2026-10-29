#ifndef A20_NET_TUN_H
#define A20_NET_TUN_H
#include "core/types.h"

struct vfile;
/* Synchronous borrowed packet view. The backend retains protocol-buffer
 * ownership; emit may copy through reader, but must not retain either pointer
 * or call back into the backend. No TUN queue lock is held on injection. */
typedef size_t (*net_tun_reader_t)(void *source, void *destination, size_t length);
typedef int (*net_tun_emit_t)(void *context, size_t length,
                             net_tun_reader_t reader, void *source);
/* Interface identities are public indices, never backend object pointers.
 * destroy serializes with emit/inject and returns only after callbacks stop. */
int tun_file_open(struct vfile *vf);
int net_tun_format(char *out, size_t capacity);
int net_tun_interface_name(unsigned index, char *out, unsigned capacity);
unsigned net_tun_interface_by_name(const char *name);
#endif
