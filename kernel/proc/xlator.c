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
 * The guest set is *derived*, never listed here: a machine is translatable
 * exactly when an administrator configured a path for it.  That is what lets
 * a new architecture be added without touching the kernel, and it is why a
 * corrupt e_machine still lands on the plain ENOEXEC path -- a garbage value
 * has no entry to find.
 */

#include "proc/xlator.h"

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
 * The registry.  XLATOR_GUEST(name, machine, argv_template); see the file's
 * header for the column contract.  Including it once keeps the guest set, the
 * e_machine numbers and the default per-translator argv shape in one place --
 * previously these were spread across a switch in mm/elf.c and a strcmp chain
 * here, which had to be edited in lockstep.
 */
typedef struct {
    const char *name;
    uint16_t    machine;
    const char *argv;
} xlator_guest_t;

#define XLATOR_GUEST(name, machine, argv) { #name, machine, argv },
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
static struct {
    int  path_set;      /* a20.xlator.<guest>= gave an absolute path */
    int  argv_valid;    /* a .argv override parsed, or none was given */
    int  env_valid;     /* a .env override parsed, or none was given */
    int  argv_overridden;
    int  env_overridden;
    char path[MAX_PATH_LEN];
    char argv[XLATOR_TMPL_LEN];
    char env[XLATOR_ENV_LEN];
} g_slots[XLATOR_GUEST_COUNT];

static int g_enabled;
static unsigned long g_forwards;

/* ================================================================== */
/*  argv template and env spec parsing                                */
/*  Both are also called from the exec path, so they are written to be  */
/*  side-effect free apart from filling @out.                          */
/* ================================================================== */

static int xlator_is_sep(char c)
{
    return c == ' ' || c == '\t' || c == ',';
}

/*
 * Split an argv template into tokens and classify each one.
 *
 * The whole vocabulary is three substitutions, and anything else beginning
 * with '@' is an error rather than literal text.  That is deliberately
 * unforgiving: a template is typed on a command line, and '@X' is far more
 * likely to be a mistyped '@A' than an argument a translator really wants.
 * Passing it through would mean the translator silently receives an argument
 * the administrator never wrote, and would only show up as a strange guest
 * failure much later.
 *
 * Requiring @P is the other half of the same bargain.  A template naming no
 * image cannot be what anyone meant, and forwarding it would exec the
 * translator with no guest to translate.
 */
int xlator_parse_template(const char *tmpl, xlator_tmpl_t *out)
{
    if (!tmpl)
        return -EINVAL;

    size_t len = strlen(tmpl);
    if (len >= XLATOR_TMPL_LEN)
        return -E2BIG;

    memset(out, 0, sizeof(*out));
    memcpy(out->text, tmpl, len);
    out->text[len] = '\0';

    int has_path = 0;
    const char *p = tmpl;

    while (*p) {
        while (*p && xlator_is_sep(*p))
            p++;
        if (!*p)
            break;

        const char *start = p;
        while (*p && !xlator_is_sep(*p))
            p++;
        size_t tlen = (size_t)(p - start);

        if (out->n >= XLATOR_TMPL_MAX)
            return -E2BIG;

        xlator_tok_kind_t kind;
        const char *text = start;
        size_t textlen = tlen;

        if (tlen == 2 && start[0] == '@') {
            switch (start[1]) {
            case 'A': kind = XLATOR_TOK_ARGV0; textlen = 0; break;
            case 'P': kind = XLATOR_TOK_PATH;  textlen = 0; has_path = 1; break;
            case '*': kind = XLATOR_TOK_SPLICE; textlen = 0; break;
            default:  return -EINVAL;   /* unknown '@' token */
            }
        } else if (tlen >= 2 && start[0] == '@' &&
                   (start[1] == 'A' || start[1] == 'P' || start[1] == '*')) {
            /* "--argv0=@A": the substitution glued to a literal prefix.  Split
             * it so the literal keeps its own argument slot -- joining the two
             * back together at exec time is the whole reason a template
             * exists, and every wrapper that takes an attached value needs
             * this. */
            if (out->n + 2 > XLATOR_TMPL_MAX)
                return -E2BIG;

            uint8_t base = (uint8_t)out->n;
            out->kind[base] = XLATOR_TOK_LITERAL;
            out->start[base] = (uint8_t)(start - tmpl);
            out->len[base] = (uint8_t)(tlen - 2);
            out->n++;

            kind = (start[1] == 'A') ? XLATOR_TOK_ARGV0
                  : (start[1] == 'P') ? XLATOR_TOK_PATH : XLATOR_TOK_SPLICE;
            if (start[1] == 'P')
                has_path = 1;
            text = NULL;
            textlen = 0;
        } else {
            kind = XLATOR_TOK_LITERAL;
            if (tlen > 255)
                return -E2BIG;   /* start/len are uint8_t */
        }

        out->kind[out->n] = (uint8_t)kind;
        if (kind == XLATOR_TOK_LITERAL) {
            out->start[out->n] = (uint8_t)(start - tmpl);
            out->len[out->n] = (uint8_t)textlen;
        } else {
            (void)text;
            out->start[out->n] = 0;
            out->len[out->n] = 0;
        }
        out->n++;
    }

    if (!has_path)
        return -EINVAL;

    return out->n;
}

/*
 * Split an env spec into "NAME=VALUE" entries, in place.
 *
 * Validation is per entry and all-or-nothing: a spec with one bad entry is
 * rejected whole, because forwarding a partial environment is the kind of
 * thing that works on the machine where it was typed and fails on the one
 * where it was not.
 */
int xlator_split_env(char *scratch, char **entries, int max)
{
    if (!scratch)
        return 0;

    int n = 0;
    char *p = scratch;

    while (*p) {
        while (*p && xlator_is_sep(*p))
            *p++ = '\0';
        if (!*p)
            break;

        char *start = p;
        while (*p && !xlator_is_sep(*p))
            p++;
        if (*p)
            *p++ = '\0';   /* the separator is also the entry terminator */

        size_t len = strlen(start);
        if (len == 0)
            continue;
        if (len > XLATOR_ENV_ENTRY_MAX)
            return -EINVAL;

        char *eq = start;
        while (*eq && *eq != '=')
            eq++;
        if (eq == start || !*eq)
            return -EINVAL;   /* no '=' at all, or an empty name */

        for (char *c = start; *c; c++) {
            if (*c < 0x20 || *c == 0x7f)
                return -EINVAL;
        }

        if (n >= max)
            return -E2BIG;
        entries[n++] = start;
    }

    return n;
}

/* ================================================================== */
/*  cmdline parsing                                                   */
/* ================================================================== */

static int xlator_slot_for_name(const char *name, size_t namelen)
{
    for (int i = 0; i < XLATOR_GUEST_COUNT; i++) {
        if (strlen(g_guests[i].name) == namelen &&
            strncmp(g_guests[i].name, name, namelen) == 0)
            return i;
    }
    return -1;
}

/* Is @name (length @namelen) a valid entry in the registry? */
static int xlator_valid_guest_name(const char *name, size_t namelen)
{
    if (namelen == 0)
        return 0;
    for (size_t i = 0; i < namelen; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            return 0;
    }
    return 1;
}

/*
 * Which of the three per-guest keys is this?
 *
 * The guest name is an identifier fragment and cannot contain a dot (the
 * registry's column-1 contract says so), so ".argv" and ".env" can be
 * recognised from the last dot alone.  Keeping that decision in one function
 * is what stops "a20.xlator.x86_64.argv" from being read as a request for a
 * guest literally named "x86_64.argv".
 *
 * Anything else falls through to PATH with the whole key as the name, so the
 * name validation downstream is what rejects "x86_64.foo" -- one place that
 * knows what a guest name may look like, not two.
 */
enum xlator_field { XL_FIELD_PATH = 0, XL_FIELD_ARGV, XL_FIELD_ENV };

static int xlator_field_of(const char *key, size_t keylen, size_t *name_len)
{
    const char *dot = NULL;
    for (size_t i = 0; i < keylen; i++) {
        if (key[i] == '.')
            dot = key + i;
    }

    *name_len = keylen;
    if (!dot || dot == key)
        return XL_FIELD_PATH;

    size_t tail = keylen - (size_t)(dot - key) - 1;
    if (tail == 4 && strncmp(dot + 1, "argv", 4) == 0) {
        *name_len = (size_t)(dot - key);
        return XL_FIELD_ARGV;
    }
    if (tail == 3 && strncmp(dot + 1, "env", 3) == 0) {
        *name_len = (size_t)(dot - key);
        return XL_FIELD_ENV;
    }
    return XL_FIELD_PATH;
}

/* Split one `<prefix><key>=<value>` token.  Returns 1 when the token carries
 * @prefix and an '=', copying the key into @keybuf (NUL-terminated, its
 * length in *@key_len) and the value into @val.
 *
 * Returns 0 both when the token is not ours and when the key does not fit in
 * @keybuf -- the caller cannot act on a truncated key, and a guest name is
 * an identifier fragment that never approaches this bound. */
static int xlator_value_of(const char *tok, const char *tok_end,
                           const char *prefix, size_t *key_len,
                           char *keybuf, size_t keysz,
                           char *val, size_t valsz)
{
    size_t plen = strlen(prefix);
    if ((size_t)(tok_end - tok) <= plen)
        return 0;
    if (strncmp(tok, prefix, plen) != 0)
        return 0;

    const char *eq = tok + plen;
    while (eq < tok_end && *eq != '=')
        eq++;
    if (eq >= tok_end)
        return 0;   /* bare "a20.xlator.something" with no value: not ours */

    *key_len = (size_t)(eq - (tok + plen));
    if (*key_len >= keysz)
        return 0;
    memcpy(keybuf, tok + plen, *key_len);
    keybuf[*key_len] = '\0';

    const char *vstart = eq + 1;
    size_t vlen = (size_t)(tok_end - vstart);
    if (vlen >= valsz)
        vlen = valsz - 1;
    memcpy(val, vstart, vlen);
    val[vlen] = '\0';
    return 1;
}

/*
 * The bare `a20.xlator=0|1` key, which enables or disables the channel.
 *
 * Handled separately from the guest keys because "a20.xlator" is a *prefix*
 * of every one of them: without the exact '=' check below,
 * `a20.xlator.x86_64=/bin/qemu-x86_64` parses as the enable key with the
 * value "/bin/x86_64", and the channel stays off while the boot log claims
 * the administrator asked for something.  That failure is silent apart from
 * a confusing warning, so the match has to be exact.
 */
static int xlator_parse_enable_key(const char *tok, const char *tok_end)
{
    size_t klen = strlen(XLATOR_ENABLE_KEY);
    if ((size_t)(tok_end - tok) <= klen)
        return 0;
    if (strncmp(tok, XLATOR_ENABLE_KEY, klen) != 0 || tok[klen] != '=')
        return 0;

    const char *vstart = tok + klen + 1;
    size_t vlen = (size_t)(tok_end - vstart);
    if (vlen >= MAX_PATH_LEN)
        vlen = MAX_PATH_LEN - 1;
    char val[MAX_PATH_LEN];
    memcpy(val, vstart, vlen);
    val[vlen] = '\0';

    if (strcmp(val, "1") == 0)
        g_enabled = 1;
    else if (strcmp(val, "0") == 0)
        g_enabled = 0;
    else
        kwarn("[XLATOR] 未知 %s='%s'，保持禁用\n", XLATOR_ENABLE_KEY, val);
    return 1;
}

/*
 * Handle one `a20.xlator.<guest>[.field]=<value>` token.  Returns 1 if it was
 * ours (including when it named a guest we do not know, which is reported and
 * then ignored), 0 if it is some other key.
 */
static int xlator_parse_guest_key(const char *tok, const char *tok_end)
{
    char key[64];
    char val[MAX_PATH_LEN];

    size_t key_len = 0;
    if (!xlator_value_of(tok, tok_end, XLATOR_GUEST_KEY, &key_len,
                         key, sizeof(key), val, sizeof(val)))
        return 0;

    /* The enable key is a20.xlator, not a20.xlator.<something>, so it cannot
     * reach here; a zero-length key means the token was "a20.xlator.=...",
     * which is a typo worth naming rather than silently dropping. */
    if (key_len == 0) {
        kwarn("[XLATOR] %s=... 缺少 guest 名，忽略\n", XLATOR_GUEST_KEY);
        return 1;
    }

    size_t name_len = 0;
    int field = xlator_field_of(key, key_len, &name_len);

    /* NUL-terminate at the guest name so a "%.*s" is not needed: this
     * kernel's klog printf has no '*' precision, and asking for one prints
     * the conversion literally -- a warning about an unknown guest that
     * never says which guest. */
    key[name_len] = '\0';

    if (!xlator_valid_guest_name(key, name_len)) {
        kwarn("[XLATOR] 未知外来架构 '%s'，忽略（见 proc/xlator_guests.def）\n",
              key);
        return 1;
    }

    int slot = xlator_slot_for_name(key, name_len);
    if (slot < 0) {
        /* An architecture the registry has never heard of.  Report it: the
         * administrator believes they provisioned a guest, and nothing will
         * ever translate it.  Silently dropping this is how a typo becomes an
         * unexplained ENOEXEC weeks later. */
        kwarn("[XLATOR] 未知外来架构 '%s'，忽略（见 proc/xlator_guests.def）\n",
              key);
        return 1;
    }

    if (field == XL_FIELD_PATH) {
        size_t vlen = strlen(val);
        if (vlen == 0 || val[0] != '/') {
            kwarn("[XLATOR] %s%s 不是绝对路径，忽略\n", XLATOR_GUEST_KEY,
                  g_guests[slot].name);
            return 1;
        }
        /* A repeated key overwrites rather than appends: last one wins, which
         * keeps the cmdline single-valued and the table bounded. */
        memcpy(g_slots[slot].path, val, vlen + 1);
        g_slots[slot].path_set = 1;
        return 1;
    }

    if (field == XL_FIELD_ARGV) {
        xlator_tmpl_t tmpl;
        int r = xlator_parse_template(val, &tmpl);
        if (r < 0) {
            /* Fail closed: mark the template unusable rather than falling back
             * to the registry default.  A translator pointed at by path and a
             * template that did not parse are a configuration the
             * administrator did not write, and guessing on their behalf is how
             * a guest ends up exec'd with the wrong argv. */
            kwarn("[XLATOR] %s%s.argv='%s' 非法（%s），该 guest 不会被转发\n",
                  XLATOR_GUEST_KEY, g_guests[slot].name, val,
                  r == -E2BIG ? "过长或 token 过多" : "未知 @ 记号或缺少 @P");
            g_slots[slot].argv_valid = 0;
            g_slots[slot].argv_overridden = 1;
            return 1;
        }
        size_t vlen = strlen(val);
        memcpy(g_slots[slot].argv, val, vlen + 1);
        g_slots[slot].argv_valid = 1;
        g_slots[slot].argv_overridden = 1;
        return 1;
    }

    /* XL_FIELD_ENV */
    char scratch[XLATOR_ENV_LEN];
    size_t vlen = strlen(val);
    if (vlen >= sizeof(scratch)) {
        kwarn("[XLATOR] %s%s.env 过长（上限 %d 字节），该 guest 不会被转发\n",
              XLATOR_GUEST_KEY, g_guests[slot].name, XLATOR_ENV_LEN - 1);
        g_slots[slot].env_valid = 0;
        return 1;
    }
    memcpy(scratch, val, vlen + 1);

    char *entries[XLATOR_ENV_MAX];
    int r = xlator_split_env(scratch, entries, XLATOR_ENV_MAX);
    if (r < 0) {
        kwarn("[XLATOR] %s%s.env='%s' 非法（%s），该 guest 不会被转发\n",
              XLATOR_GUEST_KEY, g_guests[slot].name, val,
              r == -E2BIG ? "条目过多" : "缺少 '=' 或名称为空");
        g_slots[slot].env_valid = 0;
        g_slots[slot].env_overridden = 1;
        return 1;
    }
    /* Normalise the separators to commas so /proc renders one stable form and
     * the exec path sees the same text the admin wrote.  XLATOR_ENV_LEN has
     * room for this join by construction, so the bound cannot be reached. */
    size_t off = 0;
    for (int i = 0; i < r; i++) {
        size_t elen = strlen(entries[i]);
        if (i)
            g_slots[slot].env[off++] = ',';
        memcpy(g_slots[slot].env + off, entries[i], elen);
        off += elen;
    }
    g_slots[slot].env[off] = '\0';
    g_slots[slot].env_valid = 1;
    g_slots[slot].env_overridden = 1;
    return 1;
}

void xlator_config_init(void)
{
    const char *cmdline = bootargs_get();

    const char *p = cmdline;
    while (p && *p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t')
            tok_end++;

        if (!xlator_parse_enable_key(p, tok_end))
            xlator_parse_guest_key(p, tok_end);

        p = tok_end;
    }

    /* A guest is usable only when every field it was given is valid, and the
     * registry default applies to argv/env when no override was given.  This
     * runs after the whole cmdline has been read so that the order in which
     * the administrator typed the keys cannot change the outcome. */
    for (int i = 0; i < XLATOR_GUEST_COUNT; i++) {
        if (!g_slots[i].argv_overridden) {
            memcpy(g_slots[i].argv, g_guests[i].argv, strlen(g_guests[i].argv) + 1);
            g_slots[i].argv_valid = 1;
        }
        /* An unset env means "no environment to add", which is valid -- it is
         * not the same as an .env key that failed to parse. */
        if (!g_slots[i].env_overridden)
            g_slots[i].env_valid = 1;

        /* The registry default is parsed too: a bad default must disable the
         * guest rather than reach the exec path.  On success the template is
         * rewritten into its canonical spelling, so /proc shows "@P @*" for
         * an administrator who typed "@P,@*" and for a registry default
         * alike -- the two are then comparable at a glance. */
        if (g_slots[i].argv_valid) {
            xlator_tmpl_t tmpl;
            if (xlator_parse_template(g_slots[i].argv, &tmpl) < 0) {
                g_slots[i].argv_valid = 0;
            } else {
                char norm[XLATOR_TMPL_LEN];
                size_t off = 0;
                for (int t = 0; t < tmpl.n; t++) {
                    const char *piece;
                    size_t plen;
                    switch (tmpl.kind[t]) {
                    case XLATOR_TOK_LITERAL:
                        piece = tmpl.text + tmpl.start[t];
                        plen  = tmpl.len[t];
                        break;
                    case XLATOR_TOK_ARGV0: piece = "@A"; plen = 2; break;
                    case XLATOR_TOK_PATH:  piece = "@P"; plen = 2; break;
                    default:               piece = "@*"; plen = 2; break;
                    }
                    if (off + plen + 2 >= XLATOR_TMPL_LEN)
                        break;
                    if (off)
                        norm[off++] = ' ';
                    memcpy(norm + off, piece, plen);
                    off += plen;
                }
                norm[off] = '\0';
                memcpy(g_slots[i].argv, norm, off + 1);
            }
        }
    }

    if (!g_enabled) {
        kinfo("[XLATOR] 外来架构翻译通道: 禁用\n");
        return;
    }

    int nconfigured = 0;
    for (int i = 0; i < XLATOR_GUEST_COUNT; i++) {
        if (!g_slots[i].path_set || !g_slots[i].argv_valid ||
            !g_slots[i].env_valid)
            continue;
        nconfigured++;
        kinfo("[XLATOR] %s (e_machine=%u) → %s  argv=\"%s\"%s%s%s%s\n",
              g_guests[i].name, g_guests[i].machine, g_slots[i].path,
              g_slots[i].argv,
              g_slots[i].argv_overridden ? " (cmdline 覆盖)" : "",
              g_slots[i].env[0] ? "  env=\"" : "",
              g_slots[i].env[0] ? g_slots[i].env : "",
              g_slots[i].env[0] ? "\"" : "");
    }

    if (nconfigured == 0) {
        /* Enabled but nothing to translate with: refuse loudly rather than
         * silently taking the ENOEXEC path and looking like a broken exec. */
        kwarn("[XLATOR] 已启用但未配置任何翻译器路径，execve 外来二进制仍将返回 -ENOEXEC\n");
        return;
    }

    /* Name the registered-but-unusable guests too.  Otherwise the admin who
     * typed a guest name with no path, or with a template that did not parse,
     * sees every exec fail as a bare ENOEXEC with no hint as to why. */
    for (int i = 0; i < XLATOR_GUEST_COUNT; i++) {
        if (!g_slots[i].path_set) {
            kwarn("[XLATOR] %s (e_machine=%u) 未配置翻译器，execve 将返回 -ENOEXEC\n",
                  g_guests[i].name, g_guests[i].machine);
            continue;
        }
        if (!g_slots[i].argv_valid)
            kwarn("[XLATOR] %s argv 模板非法，execve 将返回 -ENOEXEC\n",
                  g_guests[i].name);
        if (!g_slots[i].env_valid)
            kwarn("[XLATOR] %s env 规格非法，execve 将返回 -ENOEXEC\n",
                  g_guests[i].name);
    }
}

/* ================================================================== */
/*  Runtime surface                                                   */
/* ================================================================== */

int xlator_enabled(void)
{
    return __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE);
}

void xlator_set_enabled(int on)
{
    __atomic_store_n(&g_enabled, on ? 1 : 0, __ATOMIC_RELEASE);
    kinfo("[XLATOR] 通道%s（经 /proc/a20/xlator）\n", on ? "启用" : "禁用");
}

int xlator_lookup(uint16_t machine, xlator_desc_t *out)
{
    if (!xlator_enabled())
        return -ENOENT;

    for (int i = 0; i < XLATOR_GUEST_COUNT; i++) {
        if (g_guests[i].machine != machine)
            continue;
        /* path_set && argv_valid && env_valid is the same rule config_init
         * applied when it reported the boot lines; re-stating it here means
         * the exec path cannot depend on having been reached through
         * xlator_config_init(). */
        if (!g_slots[i].path_set || !g_slots[i].argv_valid ||
            !g_slots[i].env_valid)
            return -ENOENT;

        out->path = g_slots[i].path;
        out->argv = g_slots[i].argv;
        out->env  = g_slots[i].env;
        return 0;
    }
    return -ENOENT;
}

void xlator_note_forward(void)
{
    __atomic_fetch_add(&g_forwards, 1, __ATOMIC_RELAXED);
}

int xlator_render(char *buf, size_t bufsz)
{
    int n = snprintf(buf, bufsz, "enabled: %d\nforwards: %lu\nregistered: %d\n",
                     xlator_enabled(),
                     __atomic_load_n(&g_forwards, __ATOMIC_RELAXED),
                     XLATOR_GUEST_COUNT);
    if (n < 0)
        return -EINVAL;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;

    for (int i = 0; i < XLATOR_GUEST_COUNT; i++) {
        int usable = g_slots[i].path_set && g_slots[i].argv_valid &&
                     g_slots[i].env_valid;
        int w = snprintf(buf + n, bufsz - (size_t)n,
                         "guest %s: machine=%u path=%s argv=\"%s\"%s env=%s\n",
                         g_guests[i].name, g_guests[i].machine,
                         usable ? g_slots[i].path : "(none)",
                         usable ? g_slots[i].argv : "(unset)",
                         g_slots[i].argv_overridden ? " (override)" : "",
                         usable && g_slots[i].env[0] ? g_slots[i].env
                                                    : "(none)");
        if (w < 0)
            return -EINVAL;
        n += w;
        if ((size_t)n >= bufsz)
            return (int)bufsz - 1;
    }
    return n;
}

#endif /* CONFIG_XLATOR */
