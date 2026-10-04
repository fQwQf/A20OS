/*
 * A20OS — foreign-architecture translation channel
 *
 * Configuration surface for the execve hook in kernel/proc/exec.c.  The
 * kernel does not host a translator; it only knows, per guest e_machine,
 * how to hand a foreign image to a *user-space* binary.
 *
 * Three independent switches control the feature, and they compose:
 *
 *   build     CONFIG_XLATOR=0 removes the whole channel at compile time
 *             (kernel/proc/xlator.c is not even compiled).  This is the
 *             embedded / low-resource story: nothing below exists in the
 *             image, and the stubs make every entry point inert.
 *
 *   boot      a20.xlator=1 enables the channel.  Each guest architecture
 *             is then described by three keys, all resolved once at boot:
 *
 *               a20.xlator.<guest>=<path>           the translator binary
 *               a20.xlator.<guest>.argv=<template>  how to invoke it
 *               a20.xlator.<guest>.env=<K=V,...>   environment it needs
 *
 *             .argv and .env are optional and default to the registry's
 *             template and to nothing respectively.
 *
 *   runtime   /proc/a20/xlator is writable: writing 0 or 1 flips the
 *             channel without a reboot.  See xlator_set_enabled().
 *
 * The set of translatable guest architectures is *not* compiled in.  It is
 * exactly "the machines an administrator configured a path for", which is
 * why adding a guest needs no kernel change -- see proc/xlator_guests.def
 * for the registry of known guest names.
 *
 * Paths are captured at boot and never re-resolved per exec, so a process
 * cannot influence which binary the kernel will re-exec by writing to PATH
 * or to the filesystem between the check and the use.
 *
 * ---------------------------------------------------------------------------
 *  Why the invocation is a *template* and not a flag
 * ---------------------------------------------------------------------------
 *
 * The first version of this table carried a single string per guest -- the
 * option that tells a translator what the guest's argv[0] should be -- and
 * that quietly assumed every translator is qemu-user.  It is not the only
 * shape, and the wrong shape is easy to get wrong *silently*:
 *
 *   qemu-user        qemu-x86_64 -0 <argv0> <path> <args...>
 *                    argv[0] is set through an option; the guest image is a
 *                    separate positional argument.
 *
 *   Rosetta-style    rttranslator <path> <args...>
 *   (a wrapper)      no option at all: the guest image *is* argv[0].  A
 *                    per-guest flag cannot express this.  The best it could
 *                    do was suppress the flag, which still left a stray
 *                    positional argument sitting in front of the path.
 *
 *   AOT / JIT-out    a compiler, not a wrapper: it runs to produce an
 *   (Prism-style)    artifact, and the artifact is what gets exec'd.  Not
 *                    expressible as an argv template at all -- see
 *                    docs/exec-xlator/02-integration.md.
 *
 * So the convention is expressed as a template with three substitutions:
 *
 *   @A   the caller's argv[0] -- what it asked for, falling back to the
 *        path when execve was given none
 *   @P   the guest image path
 *   @*   the caller's argv[1..], spliced in place
 *
 *   "-0 @A @P @*"      qemu-user
 *   "@P @*"             a translator that wants the path first and nothing else
 *   "--argv0=@A @P @*" a translator whose option takes an attached value
 *
 * Tokens are separated by spaces, tabs or commas, which is why a cmdline
 * override is written without spaces: a20.xlator.x86_64.argv=@P,@*.  A fixed
 * translator option containing a space or a comma is therefore not
 * expressible; guest arguments are unaffected, because @* splices them
 * verbatim.
 *
 * Crucially the template is a property of the *translator binary*, not of
 * the guest architecture -- the same guest can be served by qemu-user on one
 * board and by an in-tree translator on another.  That is why it is a boot
 * key with a registry default rather than a column of the guest table: it
 * makes pointing the channel at a different kind of translator a cmdline
 * change, with no kernel edit and no table edit at all.
 */

#ifndef _A20_PROC_XLATOR_H
#define _A20_PROC_XLATOR_H

#include "core/defs.h"
#include "core/errno.h"

/* Bounds for the invocation description.  All are checked at config time, so
 * a value that would not fit is rejected at boot rather than at exec. */
#define XLATOR_TMPL_LEN        256     /* bytes in an argv template */
#define XLATOR_TMPL_MAX        32      /* tokens in an argv template */
#define XLATOR_ENV_MAX         8       /* environment entries per translator */
#define XLATOR_ENV_ENTRY_MAX   64      /* bytes in one "NAME=VALUE" entry */
/* Room for the worst-case join of XLATOR_ENV_MAX entries plus XLATOR_ENV_MAX-1
 * separating commas, so normalising separators can never overflow. */
#define XLATOR_ENV_LEN         (XLATOR_ENV_MAX * (XLATOR_ENV_ENTRY_MAX + 1))

/* What one template token expands to. */
typedef enum {
    XLATOR_TOK_LITERAL = 0,   /* fixed text passed through unchanged */
    XLATOR_TOK_ARGV0,         /* @A */
    XLATOR_TOK_PATH,          /* @P */
    XLATOR_TOK_SPLICE,        /* @* */
} xlator_tok_kind_t;

/* A parsed argv template.  Token text lives in text[] at the (start, len)
 * window, so the whole structure is one fixed-size object and the exec path
 * can keep it on the stack without a second allocation. */
typedef struct {
    char    text[XLATOR_TMPL_LEN];
    uint8_t kind[XLATOR_TMPL_MAX];
    uint8_t start[XLATOR_TMPL_MAX];
    uint8_t len[XLATOR_TMPL_MAX];
    int     n;
} xlator_tmpl_t;

/* Everything the exec hook needs to forward one guest, resolved in one
 * lookup.  The three strings point into the boot-written configuration
 * table, which is never modified afterwards -- see the "written once" note
 * in kernel/proc/xlator.c -- so borrowing them is safe and avoids copying a
 * 256-byte template into a caller buffer. */
typedef struct {
    const char *path;    /* translator binary, absolute */
    const char *argv;    /* effective argv template (override or registry) */
    const char *env;     /* "NAME=VALUE,NAME=VALUE", or "" for none */
} xlator_desc_t;

#if defined(CONFIG_XLATOR)

/* Parse the a20.xlator* keys.  Must run after bootargs_init(). */
void xlator_config_init(void);

/* True when the channel is currently on.  Reads an atomic, so it is safe to
 * call on the exec hot path from any CPU. */
int xlator_enabled(void);

/* Flip the channel at runtime (the /proc/a20/xlator write handler).
 * Takes effect for the next execve; translations already running are
 * ordinary processes by then and are deliberately left alone. */
void xlator_set_enabled(int on);

/* Resolve the translator description for @machine (an EM_* value).  Returns
 * 0 on success, or -ENOENT when the channel is disabled or has no usable
 * entry for that machine -- which the caller must treat as "fall back to
 * the ordinary ENOEXEC path".
 *
 * "No usable entry" includes a guest whose path, template or env spec was
 * rejected at boot: a misconfigured translator is never handed an argv
 * vector the administrator did not ask for. */
int xlator_lookup(uint16_t machine, xlator_desc_t *out);

/* Parse an argv template into @out.  Returns the token count, or:
 *
 *   -EINVAL  an unknown '@' token, or no @P anywhere.  Both are refused on
 *            purpose.  An unknown token is almost always a typo, and
 *            passing it through as literal text would silently hand the
 *            translator an argument nobody wrote.  A template without @P
 *            names no image to translate, so it cannot be what anyone meant.
 *   -E2BIG   more than XLATOR_TMPL_MAX tokens, or longer than
 *            XLATOR_TMPL_LEN.
 *
 * Called both at config time -- which is what keeps a malformed template off
 * the exec path -- and by the exec hook. */
int xlator_parse_template(const char *tmpl, xlator_tmpl_t *out);

/* Split an env spec ("K=V,K2=V2") into borrowed pointers.
 *
 * @scratch must be a writable NUL-terminated copy of @spec; the entries
 * point into it, so it has to outlive them.  Returns the entry count, or
 * -EINVAL for an entry with no '=', an empty name, a control character, or
 * a length over XLATOR_ENV_ENTRY_MAX, or -E2BIG past XLATOR_ENV_MAX entries.
 * Same fail-closed contract as the template: reject the whole spec rather
 * than forward a half-understood environment. */
int xlator_split_env(char *scratch, char **entries, int max);

/* Record that a translation happened.  Only called on the rare re-exec
 * path, never on an ordinary exec, so the counter costs nothing in the
 * common case. */
void xlator_note_forward(void);

/* Render state for /proc/a20/xlator: the switch, the forward count, and one
 * line per registered guest giving its machine, its configured translator,
 * and the argv template and env spec that will actually be used.  Returns
 * the number of bytes written (excluding the NUL), or a negative errno. */
int xlator_render(char *buf, size_t bufsz);

#else /* !CONFIG_XLATOR */

/* Stubs so that callers inside CONFIG_XLATOR guards are not what decides
 * whether a CONFIG_XLATOR=n build links.  The pattern mirrors
 * kernel/include/mm/swap.h: every parameter is cast out so the tree keeps
 * building under -Wall -Wextra. */

static inline void xlator_config_init(void) { }

static inline int xlator_enabled(void)
{
    return 0;
}

static inline void xlator_set_enabled(int on)
{
    (void)on;
}

static inline int xlator_lookup(uint16_t machine, xlator_desc_t *out)
{
    (void)machine;
    (void)out;
    return -ENOENT;
}

static inline void xlator_note_forward(void) { }

static inline int xlator_render(char *buf, size_t bufsz)
{
    (void)buf;
    (void)bufsz;
    return -ENOSYS;
}

#endif /* CONFIG_XLATOR */

#endif /* _A20_PROC_XLATOR_H */
