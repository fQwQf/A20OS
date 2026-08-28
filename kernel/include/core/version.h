#ifndef _VERSION_H
#define _VERSION_H

/*
 * Single source of truth for OS identity.  Both the native ABI
 * (a20_system_info) and the Linux ABI (sys_uname, /proc/version) reference
 * these macros so the values stay consistent.
 *
 * The release number uses a 20.x scheme to stay ahead of upstream Linux
 * (current ~6.x) for glibc/musl feature checks while clearly marking this
 * as A20OS, not a Linux fork.
 */
#define A20OS_SYSNAME    "A20OS"
#define A20OS_NODENAME   "a20os"
#define A20OS_VERSION    "0.12"
#define A20OS_RELEASE    "20.0.12"
#define A20OS_VERSION_FULL  A20OS_SYSNAME " " A20OS_VERSION " build " __DATE__ " " __TIME__

#endif
