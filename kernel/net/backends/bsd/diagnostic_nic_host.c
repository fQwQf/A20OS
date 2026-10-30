/* The no-NIC bootstrap diagnostic links the same isolated core but selects the
 * lwIP stack for its ordinary backend. Explicitly report absent class devices
 * to the unused ff_veth boundary; no protocol or packet operation is stubbed. */
#include "core/errno.h"
int a20_bsd_host_nic_bind(void *context, int port_id)
{ (void)context; (void)port_id; return -ENODEV; }
void a20_bsd_host_nic_unbind(void *context) { (void)context; }
int a20_bsd_host_nic_recv(void *context, void *frame, int capacity)
{ (void)context; (void)frame; (void)capacity; return -ENODEV; }
int a20_bsd_host_nic_send(void *context, const void *frame, int length)
{ (void)context; (void)frame; (void)length; return -ENODEV; }
void a20_bsd_host_nic_poll(void *context) { (void)context; }
void a20_bsd_host_nic_token_freed(void *context) { (void)context; }
