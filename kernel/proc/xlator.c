/*
 * A20OS — foreign-architecture translation channel configuration
 *
 * Boot-time capture of the a20.xlator* cmdline keys, plus the runtime switch
 * behind /proc/a20/xlator.  See kernel/include/proc/xlator.h for the
 * contract and proc/xlator_guests.def for the guest registry.
 *
 * Three invariants this file is built around:
 *
 *   - Resolution happens exactly once, at boot.  The path used to re-exec a
 *     guest is the path the administrator named on the cmdline, not
 *     something a process can steer through PATH, cwd, or a writable
 *     directory appearing between the check and the use.  Consequently the
 *     entry table is immutable after xlator_config_init() and needs no lock;
 *     only the enable flag is mutable, and that one is atomic.
 *
 *   - Fail-closed by default.  With no a20.xlator=1 -- or in a build without
 *     CONFIG_XLATOR, where this file is not compiled at all -- execve's
 *     behaviour is what it was before the feature existed.
 *
 *   - Fail-closed on a *partial* configuration too.  A guest whose argv
 *     template or env spec does not parse is treated as unconfigured, not as
 *     a guest with a default template.  The alternative -- forwarding an exec
 *     with arguments the administrator never wrote, because the interesting
 *     half of the key was a typo -- is the failure mode worth spending a
 *     warning on.
 *
 * The guest set is *derived*, never listed here: a (machine, ABI) pair is
 * translatable exactly when an administrator configured a path for it.  That
 * is what lets a new architecture -- or a new ABI for an architecture already
 * present -- be added without touching the kernel, and it is why a corrupt
 * e_machine still lands on the plain ENOEXEC path -- a garbage value has no
 * entry to find.  The pair, rather than the machine, is what is looked up:
 * the two ABIs are the same ELF64 for the same machine and the translators
 * that run them are disjoint, so a machine-only key would hand a native-ABI
 * image to a Linux-ABI translator.
 */

#include "proc/xlator.h"
#include "proc/xlator_tmpl.h"
#include "proc/xlator_config.h"

#ifdef CONFIG_XLATOR

#include "mm/elf.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/errno.h"
#include "core/consts.h"
#include "core/stdio.h"

#define XLATOR_ENABLE_KEY "a20.xlator"
#define XLATOR_GUEST_KEY  "a20.xlator."

/*
 * The registry.  XLATOR_GUEST(name, machine, abi, argv_template); see the
 * file's header for the column contract.  Including it once keeps the guest
 * set, the e_machine numbers, the ABIs and the default per-translator argv
 * shape in one place -- previously these were spread across a switch in
 * mm/elf.c and a strcmp chain here, which had to be edited in lockstep.
 */
typedef struct {
    const char *name;
    uint16_t    machine;
    uint8_t     abi;       /* elf_abi_t */
    const char *argv;
} xlator_guest_t;

/* The .def has no #include of its own -- like the syscall tables under
 * kernel/abi/ -- so it cannot say ELF_ABI_LINUX.  Naming the column with these
 * two macros puts the only place that has to know both spellings here, and
 * gives the check-xlator-guests gate a token it can cross-check against the
 * elf_abi_t enumeration. */
#define XLATOR_ABI_LINUX  ELF_ABI_LINUX
#define XLATOR_ABI_NATIVE ELF_ABI_NATIVE

#define XLATOR_GUEST(name, machine, abi, argv) { #name, machine, abi, argv },
static const xlator_guest_t g_guests[] = {
#include "proc/xlator_guests.def"
};
#undef XLATOR_GUEST

#define XLATOR_GUEST_COUNT ((int)(sizeof(g_guests) / sizeof(g_guests[0])))

/* One slot per registered guest, indexed in lockstep with g_guests.  Written
 * once by xlator_config_init() before any userspace exists, read-only after --
 * which is why no lock protects it.
 *
 * The three configuration fields are tracked separately rather than as one
 * `configured` flag so that key order on the cmdline cannot matter: whether
 * the guest is usable is computed once, at the end, from all three. */
/*
 * The registry is the only table C keeps: the Rust configuration pass reads it
 * through these accessors and owns everything derived from it.
 */
int a20_xr_count(void)
{
    return XLATOR_GUEST_COUNT;
}

const char *a20_xr_name(int index)
{
    return g_guests[index].name;
}

uint16_t a20_xr_machine(int index)
{
    return g_guests[index].machine;
}

uint8_t a20_xr_abi(int index)
{
    return g_guests[index].abi;
}

const char *a20_xr_argv(int index)
{
    return g_guests[index].argv;
}

void a20_xlator_log(int level, const char *msg)
{
    if (level)
        kwarn("%s", msg);
    else
        kinfo("%s", msg);
}

/* ================================================================== */
/*  argv template and env spec parsing                                */
/* ================================================================== */

int xlator_parse_template(const char *tmpl, xlator_tmpl_t *out)
{
    return a20_xlator_parse_template(tmpl, out);
}

int xlator_split_env(char *scratch, char **entries, int max)
{
    return a20_xlator_split_env(scratch, entries, max);
}

/* ================================================================== */
/*  Configuration and runtime surface                                 */
/* ================================================================== */

const char *xlator_abi_name(uint8_t abi)
{
    return a20_xlator_abi_name(abi);
}

int xlator_abi_parse(const char *s, size_t len, uint8_t *out)
{
    return a20_xlator_abi_parse(s, len, out);
}

void xlator_config_init(void)
{
    a20_xlator_config_init(bootargs_get());
}

int xlator_enabled(void)
{
    return a20_xlator_enabled();
}

void xlator_set_enabled(int on)
{
    a20_xlator_set_enabled(on);
    kinfo("[XLATOR] 通道%s（经 /proc/a20/xlator）\n", on ? "启用" : "禁用");
}

int xlator_lookup(const elf_guest_key_t *key, xlator_desc_t *out)
{
    if (!key || !out)
        return -ENOENT;
    return a20_xlator_lookup(key->machine, key->abi, out);
}

void xlator_note_forward(void)
{
    a20_xlator_note_forward();
}

int xlator_render(char *buf, size_t bufsz)
{
    return a20_xlator_render(buf, bufsz);
}

#endif /* CONFIG_XLATOR */
