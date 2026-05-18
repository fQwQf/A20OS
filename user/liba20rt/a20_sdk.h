/*
 * A20OS Native SDK — Unified include.
 *
 * Pull in the entire A20 Native SDK with a single #include.
 */
#ifndef _A20_SDK_H
#define _A20_SDK_H

#include "a20_types.h"
#include "a20_syscall.h"
#include "a20_handle.h"
#include "a20_event.h"
#include "a20_task.h"
#include "a20_mem.h"
#include "a20_fs.h"
#include "a20_channel.h"
#include "a20_clock.h"

/* Pure utility functions (no syscall dependency) */
#include "a20_string.h"

#endif
