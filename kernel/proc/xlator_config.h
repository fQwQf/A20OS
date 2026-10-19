#ifndef A20_PROC_XLATOR_CONFIG_H
#define A20_PROC_XLATOR_CONFIG_H

#include "core/types.h"
#include "core/consts.h"
#include "proc/xlator.h"

/*
 * C/Rust ABI for the translator configuration in kernel/proc/xlator_config.rs.
 *
 * Rust owns the cmdline key grammar, the per-guest slot table, the boot pass
 * that resolves registry defaults and normalizes templates, the runtime
 * lookup, the /proc rendering, the enable switch and the forward counter.  C
 * keeps the guest registry, the boot args source, the kernel log call and the
 * /proc file plumbing.
 */
_Static_assert(MAX_PATH_LEN == 512,
               "xlator_config.rs sizes the path slots for the hosted MAX_PATH_LEN");
_Static_assert(sizeof(xlator_desc_t) == 3 * sizeof(void *),
               "xlator_desc_t layout must match the Rust XlatorDesc");

/* The registry, read by the Rust configuration pass. */
int         a20_xr_count(void);
const char *a20_xr_name(int index);
uint16_t    a20_xr_machine(int index);
uint8_t     a20_xr_abi(int index);
const char *a20_xr_argv(int index);

/* Kernel log sink: level 0 is informational, non-zero is a warning. */
void a20_xlator_log(int level, const char *msg);

/* Rust entry points. */
void a20_xlator_config_init(const char *cmdline);
int  a20_xlator_enabled(void);
void a20_xlator_set_enabled(int on);
void a20_xlator_note_forward(void);
int  a20_xlator_lookup(uint16_t machine, uint8_t abi, xlator_desc_t *out);
int  a20_xlator_render(char *buf, size_t bufsz);
const char *a20_xlator_abi_name(uint8_t abi);
int  a20_xlator_abi_parse(const char *s, size_t len, uint8_t *out);

#endif /* A20_PROC_XLATOR_CONFIG_H */
