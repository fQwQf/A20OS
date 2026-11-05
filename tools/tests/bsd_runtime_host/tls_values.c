/* Compile this translation unit with Clang -femulated-tls. The harness calls
 * actual compiler-generated TLS accessors, not a hand-constructed descriptor. */
#ifdef A20_BSD_TEST_EXPLICIT_TLS
#include "kernel/net/backends/bsd/include/a20_bsd_explicit_tls.h"
static const unsigned initial = 17;
struct port_emutls_control port_current_thread_control = {
    sizeof(void *), __alignof__(void *), 0, 0
};
static struct port_emutls_control counter_control = {
    sizeof(unsigned), __alignof__(unsigned), 0, &initial
};
static struct port_emutls_control data_control = {257, 64, 0, 0};
unsigned *probe_counter(void)
{
    if (!pcurthread)
        pcurthread = __emutls_get_address(&counter_control);
    return (unsigned *)pcurthread;
}
unsigned char *probe_data(void) { return __emutls_get_address(&data_control); }
#else
static __thread unsigned initialized = 17;
static __thread unsigned char data[257] __attribute__((aligned(64)));
unsigned *probe_counter(void) { return &initialized; }
unsigned char *probe_data(void) { return data; }
#endif
