#ifndef _PROC_NSCLONE_H
#define _PROC_NSCLONE_H

/*
 * CLONE_NEW* namespace flag values (Linux uapi).
 *
 * They live here rather than in any one namespace's header because every
 * namespace type needs to read them -- clone/unshare/setns dispatch and
 * listns(2) classify from one source, and pidns.c needs CLONE_NEWPID without
 * dragging in the mount namespace's headers.
 */
#define LINUX_CLONE_NEWNS      0x00020000ULL
#define LINUX_CLONE_NEWCGROUP  0x02000000ULL
#define LINUX_CLONE_NEWUTS     0x04000000ULL
#define LINUX_CLONE_NEWIPC     0x08000000ULL
#define LINUX_CLONE_NEWUSER    0x10000000ULL
#define LINUX_CLONE_NEWPID     0x20000000ULL
#define LINUX_CLONE_NEWNET     0x40000000ULL

/* Not a namespace flag, but pidns_fork() needs it to tell a thread from a new
 * process: a thread inherits its leader's namespaces instead of being placed
 * in the one the leader is currently spawning children into. */
#define LINUX_CLONE_THREAD     0x00010000ULL

#endif /* _PROC_NSCLONE_H */