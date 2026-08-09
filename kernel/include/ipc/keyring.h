#ifndef _IPC_KEYRING_H
#define _IPC_KEYRING_H

/*
 * A20OS kernel keyring subsystem (ABI-independent).
 *
 * Implements the Linux key / keyring object model: named keys with a type,
 * description, permission mask and optional payload, organized into keyrings
 * that can be linked into each other.  Each task owns a session keyring; a
 * per-uid persistent user keyring is created on demand.
 *
 * The ABI layers (kernel/abi/linux/sys_keyring.c) translate the Linux wire
 * shapes and copy user strings/payloads into kernel buffers before calling
 * the entry points here.  This file never touches user pointers directly.
 *
 * Locking: a single global spinlock protects the object serial map and every
 * keyring object.  Keyrings are expected to be small and infrequently
 * modified, so a coarse lock keeps the design simple and race-free.
 */

#include "core/types.h"

struct task_t;

typedef int32_t key_serial_t;

/* Special keyring serial numbers (Linux ABI). */
#define KEY_SPEC_THREAD_KEYRING       (-1)
#define KEY_SPEC_PROCESS_KEYRING      (-2)
#define KEY_SPEC_SESSION_KEYRING      (-3)
#define KEY_SPEC_USER_KEYRING         (-4)
#define KEY_SPEC_USER_SESSION_KEYRING (-5)

/* keyctl(2) commands. */
#define KEYCTL_GET_KEYRING_ID         0
#define KEYCTL_JOIN_SESSION_KEYRING   1
#define KEYCTL_UPDATE                 2
#define KEYCTL_REVOKE                 3
#define KEYCTL_CHOWN                  4
#define KEYCTL_SETPERM                5
#define KEYCTL_DESCRIBE               6
#define KEYCTL_CLEAR                  7
#define KEYCTL_LINK                   8
#define KEYCTL_UNLINK                 9
#define KEYCTL_SEARCH                 10
#define KEYCTL_READ                   11
#define KEYCTL_INSTANTIATE            12
#define KEYCTL_NEGATE                 13
#define KEYCTL_SET_REQKEY_KEYRING     14
#define KEYCTL_SET_TIMEOUT            15
#define KEYCTL_ASSUME_AUTHORITY       16
#define KEYCTL_GET_SECURITY           17
#define KEYCTL_SESSION_TO_PARENT      18
#define KEYCTL_REJECT                 19
#define KEYCTL_INVALIDATE             20
#define KEYCTL_GET_PERSISTENT         22
#define KEYCTL_DH_COMPUTE             23
#define KEYCTL_RESTRICT_KEYRING       29
#define KEYCTL_MOVE                   31

/* Key permission bits (Linux ABI). */
#define KEY_POS_VIEW      0x01000000
#define KEY_POS_READ      0x02000000
#define KEY_POS_WRITE     0x04000000
#define KEY_POS_SEARCH    0x08000000
#define KEY_POS_LINK      0x10000000
#define KEY_POS_SETATTR   0x20000000
#define KEY_POS_ALL       0x3f000000
#define KEY_USR_VIEW      0x00010000
#define KEY_USR_READ      0x00020000
#define KEY_USR_WRITE     0x00040000
#define KEY_USR_SEARCH    0x00080000
#define KEY_USR_LINK      0x00100000
#define KEY_USR_SETATTR   0x00200000
#define KEY_USR_ALL       0x003f0000
#define KEY_GRP_VIEW      0x00000100
#define KEY_GRP_READ      0x00000200
#define KEY_GRP_WRITE     0x00000400
#define KEY_GRP_SEARCH    0x00000800
#define KEY_GRP_LINK      0x00001000
#define KEY_GRP_SETATTR   0x00002000
#define KEY_GRP_ALL       0x00003f00
#define KEY_OTH_VIEW      0x00000001
#define KEY_OTH_READ      0x00000002
#define KEY_OTH_WRITE     0x00000004
#define KEY_OTH_SEARCH    0x00000008
#define KEY_OTH_LINK      0x00000010
#define KEY_OTH_SETATTR   0x00000020
#define KEY_OTH_ALL       0x0000003f
#define KEY_ALL           0xffffffff

/* Boundary constants enforced on user inputs by the ABI layer. */
#define KEYRING_TYPE_MAX     32
#define KEYRING_DESC_MAX     256
#define KEYRING_NAME_MAX     256
#define KEYRING_PAYLOAD_MAX  (64 * 1024)
#define KEYRING_SEARCH_DEPTH 4

/* ---- Keyring object API (kernel buffers only) ---- */

/* add_key(2): add or replace a key in the ring identified by @ringid (which
 * may be a KEY_SPEC_* special serial or a positive keyring serial).  Returns
 * the key serial, or a negative errno. */
key_serial_t keyring_add_key(const char *type, const char *desc,
                             const void *payload, size_t plen,
                             key_serial_t ringid);

/* request_key(2): search the session then user keyrings for @type/@desc.
 * Returns the key serial or -ENOKEY. */
key_serial_t keyring_request_key(const char *type, const char *desc,
                                 key_serial_t dest_ringid);

/* keyctl(2) dispatch, split into per-command helpers so the ABI layer can do
 * all user pointer translation.  @out points to a kernel buffer the caller
 * owns for DESCRIBE/READ. */
key_serial_t keyring_get_keyring_id(key_serial_t id, int create);
key_serial_t keyring_join_session(const char *name); /* name may be NULL */
int  keyring_chown(key_serial_t id, int uid, int gid);
int  keyring_setperm(key_serial_t id, uint32_t perm);
int  keyring_set_timeout(key_serial_t id, unsigned long secs);
int  keyring_link(key_serial_t id, key_serial_t ringid);
int  keyring_unlink(key_serial_t id, key_serial_t ringid);
key_serial_t keyring_search(key_serial_t ringid, const char *type,
                            const char *desc);
/* DESCRIBE: writes the "type;uid;gid;perm;desc" string into @buf.  Returns
 * the full length that would have been written (excluding NUL), or a
 * negative errno. */
long keyring_describe(key_serial_t id, char *buf, size_t bufsz);
/* READ: writes the key payload (or keyring serial list) into @buf.  Returns
 * the payload length, or a negative errno. */
long keyring_read(key_serial_t id, void *buf, size_t bufsz);

/* Fork/exit hooks. */
void keyring_inherit(struct task_t *child, struct task_t *parent);
void keyring_release_task(struct task_t *t);

#endif /* _IPC_KEYRING_H */
