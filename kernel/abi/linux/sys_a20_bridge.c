/*
 * A20OS Linux ABI — channel bridge.
 *
 * Exposes the kernel's unified channel IPC (the same a20_channel_ep_t
 * objects the Native ABI references through its handle table) to Linux ABI
 * programs through file descriptors.  This is the "one mechanism, two thin
 * wrappers" boundary: a Linux program can create a channel pair or open the
 * well-known service-registry client endpoint and use plain read()/write()
 * on it, so the native service layer (registry, svcman, ...) is consumable
 * from both ABIs.
 */
#include "syscall_impl.h"
#include "core/errno.h"
#include "core/fcntl.h"
#include "ipc/ipc.h"
#include "ipc/channel_fd.h"

/*
 * SYS_a20_channel_pair(int fds[2]) — create a channel pair and return both
 * endpoints as file descriptors (socketpair-style).  Messages written on one
 * end are received whole on the other (SOCK_SEQPACKET semantics).
 */
int64_t sys_a20_channel_pair(const linux_syscall_args_t *args)
{
    int *fds = (int *)(uintptr_t)args->arg[0];
    if (!fds)
        return -EFAULT;

    a20_channel_ep_t *ep0 = a20_channel_create(0, NULL);
    if (!ep0)
        return -ENOMEM;
    a20_channel_ep_t *ep1 = ep0->peer;

    int a = a20_channel_fd_install(ep0, O_RDWR);
    if (a < 0) {
        a20_channel_ep_release(ep0);
        a20_channel_ep_release(ep1);
        return a;
    }
    int b = a20_channel_fd_install(ep1, O_RDWR);
    if (b < 0) {
        vfs_close(a);
        a20_channel_ep_release(ep1);
        return b;
    }

    int out[2] = { a, b };
    if (copy_to_user(fds, out, sizeof(out)) < 0) {
        vfs_close(a);
        vfs_close(b);
        return -EFAULT;
    }
    return 0;
}

/*
 * SYS_a20_registry_client(void) — return the well-known service-registry
 * client endpoint (the same one installed in every Native task's start_info)
 * as a file descriptor.  A Linux program uses read()/write() on it to issue
 * registry RPCs to the supervisor (svcman).
 */
int64_t sys_a20_registry_client_fd(const linux_syscall_args_t *args)
{
    (void)args;
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    extern a20_channel_ep_t *a20_registry_client_ep(void);
    a20_channel_ep_t *ep = a20_registry_client_ep();
    if (!ep)
        return -ENOSYS;
    a20_object_ref(ep, A20_OBJ_CHANNEL_ENDPOINT);
    return a20_channel_fd_install(ep, O_RDWR);
#else
    return -ENOSYS;
#endif
}
