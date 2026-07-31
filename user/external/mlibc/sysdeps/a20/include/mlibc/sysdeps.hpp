#pragma once

#include <mlibc/sysdep-signatures.hpp>

namespace mlibc {

/*
 * A20OS native ABI sysdep coverage.
 *
 * Deliberately absent (by native ABI design, docs/native-abi/00-overview.md):
 *  - no fork/execve: task_spawn + posix_spawn is the process model
 *  - no signals: sigaction/sigprocmask/kill are inert compatibility stubs
 *  - no epoll/signalfd/timerfd: event_queue is the native replacement
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
	SetSockopt
{};

template<typename Tag>
using Sysdeps = SysdepOf<A20SysdepTags, Tag>;

struct SysdepTraits {
	static constexpr bool usesRtNetlink = false;
};

} // namespace mlibc
