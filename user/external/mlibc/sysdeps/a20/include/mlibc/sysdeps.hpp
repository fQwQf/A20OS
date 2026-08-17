#pragma once

#include <mlibc/sysdep-signatures.hpp>

namespace mlibc {

/*
 * A20OS native ABI sysdep coverage.
 *
 * The native ABI deliberately has no POSIX fork/execve: task_spawn and the
 * capability-safe task_clone continuation are the process model, and
 * execve is provided in-place for the fork() child (see Sysdeps<Fork>).
 * Signals are checkpoint-style compatibility stubs; epoll/signalfd/timerfd
 * are replaced by event_queue.
 */
struct A20SysdepTags :
	LibcLog,
	LibcPanic,
	TcbSet,
	FutexTid,
	FutexWait,
	FutexWake,
	AnonAllocate,
	AnonFree,
	Open,
	Openat,
	OpenDir,
	ReadEntries,
	Read,
	Write,
	Pread,
	Pwrite,
	Readv,
	Writev,
	Seek,
	Close,
	Stat,
	VmMap,
	VmUnmap,
	VmProtect,
	Exit,
	ThreadExit,
	Clone,
	PrepareStack,
	ClockGet,
	ClockGetres,
	Sleep,
	Yield,
	Isatty,
	Access,
	Faccessat,
	Dup,
	Dup2,
	Fcntl,
	Ftruncate,
	Truncate,
	Rmdir,
	Unlinkat,
	Rename,
	Renameat,
	Mkdir,
	Mkdirat,
	Link,
	Linkat,
	Symlink,
	Symlinkat,
	Readlink,
	Readlinkat,
	Fsync,
	Fdatasync,
	Sync,
	GetPid,
	GetTid,
	GetPpid,
	GetPgid,
	GetUid,
	GetEuid,
	GetGid,
	GetEgid,
	Fork,
	Execve,
	Waitpid,
	Kill,
	Sigprocmask,
	Sigaction,
	Sigsuspend,
	Sigpending,
	Sigaltstack,
	Tcgetattr,
	Tcsetattr,
	Tcsendbreak,
	Tcdrain,
	Tcflow,
	Tcflush,
	Tcgetwinsize,
	Tcsetwinsize,
	Pipe,
	GetCwd,
	Chdir,
	Fchdir,
	Umask,
	Uname,
	GetHostname,
	SetHostname,
	GetEntropy,
	GetRlimit,
	GetRusage,
	Sysconf,
	Poll,
	Ppoll,
	Pselect,
	Socket,
	Socketpair,
	Bind,
	Connect,
	Listen,
	Accept,
	Sockname,
	Peername,
	MsgSend,
	MsgRecv,
	Sendto,
	Recvfrom,
	Shutdown,
	GetSockopt,
	SetSockopt,
	Ioctl
{};

template<typename Tag>
using Sysdeps = SysdepOf<A20SysdepTags, Tag>;

struct SysdepTraits {
	static constexpr bool usesRtNetlink = false;
};

} // namespace mlibc
