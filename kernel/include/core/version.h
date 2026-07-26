#ifndef _VERSION_H
#define _VERSION_H

/*
 * Keep the project release separate from the Linux ABI release.  Native
 * A20OS interfaces report A20OS_VERSION, while Linux-compatible interfaces
 * must expose a version new enough for the glibc binaries they execute.
 */
#define A20OS_VERSION "0.5"
#define LINUX_ABI_RELEASE "6.8.0-a20"
#define LINUX_ABI_VERSION "#1 A20OS " A20OS_VERSION

/* Backwards-compatible name used by the native A20OS ABI. */
#define VERSION A20OS_VERSION

#endif
