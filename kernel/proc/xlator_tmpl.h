#ifndef A20_PROC_XLATOR_TMPL_H
#define A20_PROC_XLATOR_TMPL_H

#include "core/types.h"
#include "proc/xlator.h"

/*
 * C/Rust ABI for the translator invocation parsers in
 * kernel/proc/xlator_tmpl.rs.  Rust owns the argv-template tokenizer and the
 * env-spec splitter; both are pure functions over caller buffers, called at
 * boot to validate the configuration and again on the forwarding path.
 *
 * The template object is the public xlator_tmpl_t the exec path keeps on its
 * stack; the Rust mirror is byte-compatible and asserts the same size.
 */
_Static_assert(sizeof(xlator_tmpl_t) ==
                   3 * XLATOR_TMPL_MAX + XLATOR_TMPL_LEN + sizeof(int),
               "xlator_tmpl_t layout must match the Rust XlatorTmpl");

int a20_xlator_parse_template(const char *tmpl, xlator_tmpl_t *out);
int a20_xlator_split_env(char *scratch, char **entries, int max);

#endif /* A20_PROC_XLATOR_TMPL_H */
