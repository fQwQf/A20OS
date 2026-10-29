#ifndef A20_BSD_RUNTIME_TLS_H
#define A20_BSD_RUNTIME_TLS_H
#include "core/types.h"
struct port_tls_context;
/* Bind only on the owning kernel task; IRQs must stage work, never enter BSD.
 * Unbind after callbacks stop, then destroy. No descriptor changes or user TLS. */
struct port_tls_context *port_tls_create(void);
int port_tls_bind(struct port_tls_context *context);
int port_tls_unbind(struct port_tls_context *context);
int port_tls_destroy(struct port_tls_context *context);
#endif
