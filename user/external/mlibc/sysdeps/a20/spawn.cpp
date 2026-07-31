/*
 * posix_spawn attribute / file-action helpers for the A20 port.
 * Syscall-free; the spawn itself lives in sysdeps.cpp (task_spawn based).
 */
#include <errno.h>
#include <fcntl.h>
#include <mlibc/allocator.hpp>
#include <mlibc/spawn-types.hpp>
#include <spawn.h>
#include <utility>
#include <stdlib.h>
#include <string.h>

static __mlibc_spawnattr *attr_alloc() {
	auto *a = (__mlibc_spawnattr *)calloc(1, sizeof(__mlibc_spawnattr));
	return a;
}

int posix_spawnattr_init(posix_spawnattr_t *attr) {
	auto *a = attr_alloc();
	if (!a)
		return ENOMEM;
	attr->__heap_ptr = a;
	return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr) {
	free(attr->__heap_ptr);
	attr->__heap_ptr = nullptr;
	return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *__restrict attr, short *__restrict flags) {
	auto *a = __mlibc_spawnattr::from(attr);
	*flags = (short)(a ? a->__flags : 0);
	return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags) {
	auto *a = __mlibc_spawnattr::from(attr);
	if (!a)
		return EINVAL;
	a->__flags = flags;
	return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t *__restrict attr, pid_t *__restrict pgrp) {
	auto *a = __mlibc_spawnattr::from(attr);
	*pgrp = a ? a->__pgrp : 0;
	return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgrp) {
	auto *a = __mlibc_spawnattr::from(attr);
	if (!a)
		return EINVAL;
	a->__pgrp = pgrp;
	return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t *__restrict attr, sigset_t *__restrict set) {
	auto *a = __mlibc_spawnattr::from(attr);
	if (a)
		memcpy(set, &a->__def, sizeof(sigset_t));
	else
		memset(set, 0, sizeof(sigset_t));
	return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *__restrict attr, const sigset_t *__restrict set) {
	auto *a = __mlibc_spawnattr::from(attr);
	if (!a)
		return EINVAL;
	memcpy(&a->__def, set, sizeof(sigset_t));
	return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t *__restrict attr, sigset_t *__restrict set) {
	auto *a = __mlibc_spawnattr::from(attr);
	if (a)
		memcpy(set, &a->__mask, sizeof(sigset_t));
	else
		memset(set, 0, sizeof(sigset_t));
	return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *__restrict attr, const sigset_t *__restrict set) {
	auto *a = __mlibc_spawnattr::from(attr);
	if (!a)
		return EINVAL;
	memcpy(&a->__mask, set, sizeof(sigset_t));
	return 0;
}

int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *__restrict attr, int *__restrict pol) {
	auto *a = __mlibc_spawnattr::from(attr);
	*pol = a ? a->__pol : 0;
	return 0;
}

int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int pol) {
	auto *a = __mlibc_spawnattr::from(attr);
	if (!a)
		return EINVAL;
	a->__pol = pol;
	return 0;
}

int posix_spawnattr_getschedparam(const posix_spawnattr_t *__restrict attr, struct sched_param *__restrict param) {
	auto *a = __mlibc_spawnattr::from(attr);
	if (a)
		memcpy(param, &a->__schedparam, sizeof(*param));
	else
		memset(param, 0, sizeof(*param));
	return 0;
}

int posix_spawnattr_setschedparam(posix_spawnattr_t *__restrict attr, const struct sched_param *__restrict param) {
	auto *a = __mlibc_spawnattr::from(attr);
	if (!a)
		return EINVAL;
	memcpy(&a->__schedparam, param, sizeof(*param));
	return 0;
}

/* ---- file actions ---- */

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) {
	auto *a = (__mlibc_spawn_file_actions *)calloc(1, sizeof(__mlibc_spawn_file_actions));
	if (!a)
		return ENOMEM;
	new (a) __mlibc_spawn_file_actions();
	fa->__heap_ptr = a;
	return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
	auto *a = __mlibc_spawn_file_actions::from(fa);
	if (a) {
		a->~__mlibc_spawn_file_actions();
		free(a);
	}
	fa->__heap_ptr = nullptr;
	return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
	auto *a = __mlibc_spawn_file_actions::from(fa);
	if (!a || fd < 0)
		return EINVAL;
	__mlibc_spawn_file_actions::fdop op{};
	op.cmd = 1; /* FDOP_CLOSE */
	op.fd = fd;
	a->ops.push_back(std::move(op));
	return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int srcfd, int fd) {
	auto *a = __mlibc_spawn_file_actions::from(fa);
	if (!a || srcfd < 0 || fd < 0)
		return EINVAL;
	__mlibc_spawn_file_actions::fdop op{};
	op.cmd = 2; /* FDOP_DUP2 */
	op.fd = fd;
	op.srcfd = srcfd;
	a->ops.push_back(std::move(op));
	return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *__restrict path, int oflag, mode_t mode) {
	auto *a = __mlibc_spawn_file_actions::from(fa);
	if (!a || fd < 0 || !path)
		return EINVAL;
	__mlibc_spawn_file_actions::fdop op{};
	op.cmd = 3; /* FDOP_OPEN */
	op.fd = fd;
	op.oflag = oflag;
	op.mode = mode;
	op.path = frg::string<MemoryAllocator>{getAllocator(), path};
	a->ops.push_back(std::move(op));
	return 0;
}

int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *fa, const char *path) {
	(void)fa; (void)path;
	return ENOSYS;
}

int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *fa, int fd) {
	(void)fa; (void)fd;
	return ENOSYS;
}
