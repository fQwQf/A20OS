#ifndef _CORE_POLL_H
#define _CORE_POLL_H

/*
 * Kernel-internal poll.h constant namespace.
 * Values intentionally match the Linux ABI wire format; the ABI layer
 * re-exports them (abi/linux/...).  Internal code must include this
 * header, never anything under abi/.
 */
#define POLLIN         0x001
#define POLLPRI        0x002
#define POLLOUT        0x004
#define POLLERR        0x008
#define POLLHUP        0x010
#define POLLNVAL       0x020

#endif
