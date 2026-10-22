/*
 * A20OS — Process execution (proc_exec)
 *
 * Replaces the current process image with a new program loaded from the
 * filesystem.  Supports:
 *   - ELF64 executables (static & dynamically linked)
 *   - Script files with shebang (#!) interpreters, iteratively resolved
 *   - Dual ABI: Linux (argc/argv/envp/auxv) and Native (a20_start_info_t)
 *
 * Architecture:
 *   proc_exec() copies user args once into an exec_bprm, then iterates
 *   up to EXEC_MAX_DEPTH times trying first ELF then shebang.  No
 *   recursion — bounded stack usage, single point of cleanup.
 *
 * Design inspired by Linux's do_execve / struct linux_binprm.
 */

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "proc/debug.h"
#include "proc/xlator.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "fs/vfs/path.h"
#include "ipc/landlock.h"
#include "mm/elf.h"
#include "mm/mm.h"
#include "mm/vdso.h"
#include "mm/vm.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/trap.h"
#include "sys/usercopy.h"
#include "mm/elf_rust.h"
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
#include "ipc/ipc.h"
#include "ipc/start_info.h"
#include "ipc/handle_table.h"

extern a20_channel_ep_t *a20_registry_client_ep(void);

struct a20_ht_internal;
struct a20_ht_internal *a20_ht_create(void);
void a20_ht_put_ref(struct a20_ht_internal *ht);
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
#endif

/* ================================================================== */
/*  Constants                                                         */
/* ================================================================== */

#define EXEC_MAX_DEPTH  4       /* max shebang nesting (Linux: BINPRM_MAX_RECURSION) */
#define EXEC_RETRY      1       /* return code: try again with rewritten bprm */
#define EXEC_DONE       0       /* return code: ELF loaded, install it */

/* ================================================================== */
/*  exec_bprm — central state for an in-progress exec                 */
/* ================================================================== */

typedef struct {
    char   *path;                       /* current executable path (kalloc'd) */
    char   *args[MAX_ARG_STRINGS + 1];  /* kernel copies of argv strings */
    char   *envs[MAX_ARG_STRINGS + 1];  /* kernel copies of envp strings */
    int     argc;
    int     envc;
    int     depth;                      /* shebang nesting depth */
    size_t  bytes;                      /* argv+envp string bytes, for limits */
} exec_bprm_t;

/* ================================================================== */
/*  Helpers: credential application (unchanged logic)                  */
/* ================================================================== */

static void proc_apply_exec_creds(task_t *t, const kstat_t *st)
{
    if (!t || !st)
        return;

    /* The transition decision (set-id elevation, no_new_privs stickiness,
     * capability reconstruction, dumpable flip) is Rust-owned; this applies
     * the planned values to the task credential. */
    a20_elf_cred_input_t in = {
        .no_new_privs  = t->policy.no_new_privs ? 1 : 0,
        .uid           = t->cred.uid,
        .euid          = t->cred.euid,
        .suid          = t->cred.suid,
        .egid          = t->cred.egid,
        .fsuid         = t->cred.fsuid,
        .fsgid         = t->cred.fsgid,
        .sgid          = t->cred.sgid,
        .st_uid        = (int32_t)st->st_uid,
        .st_gid        = (int32_t)st->st_gid,
        .st_mode       = st->st_mode,
        .cap_bounding  = t->cred.cap_bounding,
        .cap_permitted = t->cred.cap_permitted,
        .cap_effective = t->cred.cap_effective,
    };
    a20_elf_cred_plan_t p;
    if (a20_elf_plan_creds(&in, &p) < 0)
        return; /* the planner cannot fail; defensive only */
    t->cred.euid          = p.euid;
    t->cred.suid          = p.suid;
    t->cred.fsuid         = p.fsuid;
    t->cred.egid          = p.egid;
    t->cred.fsgid         = p.fsgid;
    t->cred.sgid          = p.sgid;
    t->cred.cap_permitted = p.cap_permitted;
    t->cred.cap_effective = p.cap_effective;
    /* SECBIT_KEEP_CAPS is documented as "cleared on execve()" (securebits.h):
     * it exists so a program can prctl it on, change uid and drop caps inside
     * one process, not so it survives into the next program.  A locked bit
     * stays, the same way the *_LOCKED bits freeze the rest of the policy. */
    if (!(t->cred.securebits & (SECBIT_KEEP_CAPS << 1)))
        t->cred.securebits &= ~SECBIT_KEEP_CAPS;
    if (p.make_undumpable)
        mm_set_dumpable(t->mm, 0);
}

/* ================================================================== */
/*  bprm lifecycle                                                    */
/* ================================================================== */

static void bprm_free_strings(exec_bprm_t *bprm)
{
    for (int i = 0; i < bprm->argc; i++) {
        kfree(bprm->args[i]);
        bprm->args[i] = NULL;
    }
    for (int i = 0; i < bprm->envc; i++) {
        kfree(bprm->envs[i]);
        bprm->envs[i] = NULL;
    }
    bprm->argc = 0;
    bprm->envc = 0;
}

static void bprm_free(exec_bprm_t *bprm)
{
    bprm_free_strings(bprm);
    if (bprm->path) {
        kfree(bprm->path);
        bprm->path = NULL;
    }
}

static int exec_replace_path(exec_bprm_t *bprm, const char *path)
{
    char *copy = kmalloc(strlen(path) + 1);
    if (!copy)
        return -ENOMEM;
    strcpy(copy, path);
    kfree(bprm->path);
    bprm->path = copy;
    return 0;
}

/* ================================================================== */
/*  exec_copy_args — copy argv OR envp from user into kernel arrays   */
/* ================================================================== */

/*
 * Copy an array of string pointers from user space into kernel-owned
 * copies.  proc_exec() is an ABI syscall boundary, so argv and envp always
 * retain user-pointer provenance even on identity-mapped architectures.
 *
 * @src        pointer to the user pointer array, or NULL
 * @out        pre-allocated array of char* (at least MAX_ARG_STRINGS+1)
 * @out_count  output: number of strings copied
 * @arg_bytes  in/out: running total of bytes (for limit check)
 * @max_bytes  maximum allowed total (stack / 4)
 *
 * Returns 0 on success, negative errno on failure.
 * On failure, any partially copied strings are freed.
 */
static int exec_copy_args(char *const *src, char **out, int *out_count,
                          size_t *arg_bytes, size_t max_bytes)
{
    task_t *t = proc_current();
    int count = 0;

    if (!src) {
        out[0] = NULL;
        *out_count = 0;
        return 0;
    }

    while (count < MAX_ARG_STRINGS) {
        char *ptr;
        if (copy_from_user(&ptr, &src[count], sizeof(char *)) < 0) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -EFAULT;
        }
        if (!ptr)
            break;

        /* Match the old user_strncpy(..., MAX_ARG_STRLEN) boundary: inspect
         * at most MAX_ARG_STRLEN - 1 source bytes, then report E2BIG without
         * probing the first rejected byte. */
        long measured = user_strnlen(ptr, MAX_ARG_STRLEN - 1);
        if (measured < 0) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return (int)measured;
        }
        if ((size_t)measured >= MAX_ARG_STRLEN - 1) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -E2BIG;
        }
        size_t len = (size_t)measured + 1;
        /* Byte-budget decision is Rust-owned (per-string cap + aggregate). */
        long next = a20_elf_check_arg_bytes(*arg_bytes, len,
                                            MAX_ARG_STRLEN, max_bytes,
                                            t ? (uint64_t)(t->limits.stack / 4) : 0);
        if (next < 0) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return (int)next;
        }

        out[count] = kmalloc(len);
        if (!out[count]) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -ENOMEM;
        }

        if (copy_from_user(out[count], ptr, len) < 0) {
            kfree(out[count]); out[count] = NULL;
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -EFAULT;
        }
        if (out[count][len - 1] != '\0') {
            kfree(out[count]); out[count] = NULL;
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -EFAULT;
        }

        *arg_bytes = (size_t)next;
        count++;
    }

    /* Check for overflow: if the array has more than MAX_ARG_STRINGS entries */
    if (count == MAX_ARG_STRINGS) {
        char *extra;
        if (copy_from_user(&extra, &src[count], sizeof(char *)) < 0) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -EFAULT;
        }
        if (extra) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -E2BIG;
        }
    }

    out[count] = NULL;
    *out_count = count;
    return 0;
}

/* ================================================================== */
/*  exec_open_and_check — open file, validate type & permissions      */
/* ================================================================== */

/*
 * Opens the executable, checks it's a regular file with execute
 * permission, and returns the fd.  Also fills @st_out if non-NULL.
 *
 * Returns fd >= 0 on success, negative errno on failure.
 */
static int exec_open_and_check(const char *path, kstat_t *st_out,
                               char resolved[MAX_PATH_LEN])
{
    int lr = landlock_check_path(path, LANDLOCK_ACCESS_FS_READ_FILE);
    if (lr < 0)
        return lr;
    /* openat2 records the path after following all symlinks on the very file
     * opened, avoiding a separate realpath walk racing a rename/replacement. */
    int fd = vfs_openat2(AT_FDCWD, path, O_RDONLY, 0, 0);
    if (fd < 0)
        return fd;

    vfile_t *vf = fdtable_get_current_file_ref(fd);
    if (!vf) {
        vfs_close(fd);
        return -EBADF;
    }
    strncpy(resolved, vf->path, MAX_PATH_LEN - 1);
    resolved[MAX_PATH_LEN - 1] = '\0';
    vfs_put_file(vf);

    kstat_t st;
    int sr = vfs_fstat(fd, &st);
    if (sr != 0) {
        vfs_close(fd);
        return sr < 0 ? sr : -EIO;
    }
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        vfs_close(fd);
        return -EACCES;
    }
    if ((st.st_mode & S_IFMT) != S_IFREG) {
        vfs_close(fd);
        return -EACCES;
    }
    if (st.st_mode & 0111) {
        int xr = vfs_faccessat2(AT_FDCWD, path, X_OK, AT_EACCESS);
        if (xr < 0) {
            vfs_close(fd);
            return xr;
        }
    } else {
        /* No execute bit — only allowed if it's a script (checked later) */
    }
    if (st_out)
        *st_out = st;

    return fd;
}

/* ================================================================== */
/*  exec_try_script — detect shebang and rewrite bprm for interpreter */
/* ================================================================== */

/*
 * Checks if @fd is a script (starts with #!).  If so, reads the
 * interpreter path and optional argument, rewrites bprm->path and
 * bprm->args for the interpreter, and returns EXEC_RETRY.
 *
 * If not a script, returns 0 (caller should try ELF).
 * On error, returns negative errno.
 */
static int exec_try_script(int fd, exec_bprm_t *bprm)
{
    /* elf_load() probes the same open file description first and leaves its
     * offset after the ELF header.  Shebang recognition must always start at
     * byte zero; otherwise direct execve() of every script is misclassified
     * as a non-script and returns ENOEXEC. */
    int seek_result = vfs_lseek(fd, 0, SEEK_SET);
    if (seek_result < 0)
        return seek_result;

    char magic[2];
    int n = vfs_read(fd, magic, sizeof(magic));
    if (n < 0)
        return n;
    if (n < 2 || magic[0] != '#' || magic[1] != '!')
        return 0;   /* not a script, caller should try ELF */

    /* Read the full first line (up to 256 bytes) */
    char buf[256];
    seek_result = vfs_lseek(fd, 0, SEEK_SET);
    if (seek_result < 0)
        return seek_result;
    n = vfs_read(fd, buf, sizeof(buf) - 1);
    if (n < 0)
        return n;
    if (n < 2)
        return -ENOEXEC;

    /* Field extraction (interpreter path, optional argument) and the argv
     * rewrite shape are Rust-owned; the bprm ownership dance stays here. */
    a20_elf_shebang_plan_t sbp;
    if (a20_elf_plan_shebang((const uint8_t *)buf, (size_t)n, &sbp) < 0)
        return -ENOEXEC;
    if (!sbp.is_script)
        return 0;   /* not a script, caller should try ELF */

    size_t ilen = sbp.interp_len;
    if (ilen == 0 || ilen >= MAX_PATH_LEN)
        return -ENOEXEC;

    char interp_path[MAX_PATH_LEN];
    memcpy(interp_path, buf + sbp.interp_off, ilen);
    interp_path[ilen] = '\0';

    char interp_arg[MAX_PATH_LEN];
    interp_arg[0] = '\0';
    if (sbp.arg_len > 0 && sbp.arg_len < MAX_PATH_LEN) {
        memcpy(interp_arg, buf + sbp.arg_off, sbp.arg_len);
        interp_arg[sbp.arg_len] = '\0';
    }

    /*
     * Build new argv:  interp_path [interp_arg] bprm->path [original args from index 1...]
     *
     * We reuse bprm->args[] in place.  Strategy:
     *   1. The original bprm->path is the script's path (becomes argv[1] or argv[2]).
     *   2. Free the old strings that won't be used, shift the rest.
     */
    int new_argc = 0;
    char *new_args[MAX_ARG_STRINGS + 1];

    /* interp_path (argv[0]) */
    new_args[new_argc] = kmalloc(strlen(interp_path) + 1);
    if (!new_args[new_argc]) goto nomem;
    strcpy(new_args[new_argc], interp_path);
    new_argc++;

    /* optional interp_arg (argv[1]) */
    if (interp_arg[0]) {
        new_args[new_argc] = kmalloc(strlen(interp_arg) + 1);
        if (!new_args[new_argc]) goto nomem;
        strcpy(new_args[new_argc], interp_arg);
        new_argc++;
    }

    /* script path (the original bprm->path) */
    new_args[new_argc] = kmalloc(strlen(bprm->path) + 1);
    if (!new_args[new_argc]) goto nomem;
    strcpy(new_args[new_argc], bprm->path);
    new_argc++;

    /* original args from index 1 onward (skip argv[0] which was the script) */
    /* The rewrite shape is Rust-owned: [interp, (arg), script, argv[1..]]
     * capped at MAX_ARG_STRINGS total. */
    a20_elf_shebang_argv_plan_t ap;
    a20_elf_plan_shebang_argv(interp_arg[0] != '\0', (size_t)bprm->argc,
                              MAX_ARG_STRINGS, &ap);
    int new_cap = (int)ap.new_argc;
    for (int i = 1; i < bprm->argc && new_argc < new_cap; i++) {
        new_args[new_argc++] = bprm->args[i];
        bprm->args[i] = NULL;   /* ownership transferred, don't free */
    }
    new_args[new_argc] = NULL;

    /* Free old argv[0] (the original script path in args) and old bprm->path */
    if (bprm->args[0]) { kfree(bprm->args[0]); bprm->args[0] = NULL; }
    if (bprm->path) { kfree(bprm->path); bprm->path = NULL; }

    /* Install new state */
    memcpy(bprm->args, new_args, (new_argc + 1) * sizeof(char *));
    bprm->argc = new_argc;

    /* Set new path to interpreter */
    bprm->path = kmalloc(strlen(interp_path) + 1);
    if (!bprm->path) goto nomem;
    strcpy(bprm->path, interp_path);

    return EXEC_RETRY;

nomem:
    for (int i = 0; i < new_argc; i++)
        kfree(new_args[i]);
    return -ENOMEM;
}

/* ================================================================== */
/*  exec_try_translator — foreign-architecture binary -> user-space   */
/*  translator, by in-place re-exec                                   */
/* ================================================================== */

#ifdef CONFIG_XLATOR

/*
 * Last-resort handler for an execve target that elf_load() rejected with
 * -ENOEXEC but which elf_probe_foreign() recognises as a well-formed
 * foreign-architecture ELF (see kernel/mm/elf.c).
 *
 * The rewrite is deliberately *re-exec*, not service forwarding: exec
 * requires the caller to become the target image, and a wrapper-style
 * translator is an ordinary Linux-ABI program that runs the guest inside
 * its own address space.  So the calling task loads the translator natively
 * and the guest becomes its argument vector -- indistinguishable, to the
 * scheduler, the fd table and /proc, from having run natively.
 *
 * New argv, matching exec_try_script()'s ownership dance:
 *
 *     [0]  translator path
 *     [1..] the configured template, expanded
 *
 * The template is where the translator's own conventions live, and it is
 * looked up per guest rather than hardcoded.  qemu-user spells it
 * "-0 @A @P @*": guests dispatch on argv[0] (busybox only enters applet mode
 * when argv[0] contains "busybox"), so the caller's argv[0] has to be
 * handed over explicitly, and the guest image is a *separate* positional
 * argument.  A wrapper that instead wants the image path as argv[0] itself
 * is configured "@P @*" and gets exactly that -- no stray argument in front
 * of the path, which is what the earlier per-guest "argv0 flag" column
 * could not express.  See kernel/include/proc/xlator.h.
 *
 * Two properties the rewrite preserves deliberately:
 *
 *   - argv[0] of the *guest* is what the caller asked for, when the
 *     template propagates it, so /proc/self/cmdline and anything
 *     dispatching on the program name keep working.
 *   - The translator is a host-native image, resolved once at boot.  A
 *     process cannot redirect the re-exec through PATH or a writable
 *     directory, and the EXEC_RETRY depth cap stops a misconfiguration that
 *     pointed the channel at a foreign-architecture binary from looping.
 *
 * Returns EXEC_RETRY to re-enter the resolution loop with the rewritten
 * bprm, 0 when the file is not translatable (caller should keep looking),
 * or a negative errno.
 */

/* Copy @src into the next free slot of @out, or fail.  Keeps the ownership
 * rules in one place: everything in @out is owned by the caller and freed by
 * the single cleanup path, whether it was allocated here or transferred from
 * the bprm. */
static int xlator_push(char **out, int *n, int max, const char *src)
{
    if (*n >= max)
        return -E2BIG;
    size_t len = strlen(src) + 1;
    char *copy = kmalloc(len);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, src, len);
    out[(*n)++] = copy;
    return 0;
}

/*
 * Expand one template token into @out.
 *
 * XLATOR_TOK_SPLICE differs from the others in ownership: it hands over the
 * caller's own argv[1..] pointers instead of copying them, because a guest
 * with a long argument vector should not pay for a second copy of it on a
 * path that is already rare.  The handover nulls the source slot, so the
 * bprm can no longer free them and exactly one side ends up owning each.
 */
static int xlator_push_token(const xlator_tmpl_t *tmpl, int i,
                             exec_bprm_t *bprm, char **out, int *n, int max)
{
    switch (tmpl->kind[i]) {
    case XLATOR_TOK_LITERAL: {
        char lit[XLATOR_TMPL_LEN];
        memcpy(lit, tmpl->text + tmpl->start[i], tmpl->len[i]);
        lit[tmpl->len[i]] = '\0';
        return xlator_push(out, n, max, lit);
    }
    case XLATOR_TOK_ARGV0:
        /* The argv[0] the caller passed.  It may be absent or not match
         * bprm->path (execve lets them disagree); honour what the caller
         * asked for, and fall back to the path only when there is no
         * argv[0] at all. */
        return xlator_push(out, n, max,
                           (bprm->argc > 0 && bprm->args[0]) ? bprm->args[0]
                                                             : bprm->path);
    case XLATOR_TOK_PATH:
        return xlator_push(out, n, max, bprm->path);
    case XLATOR_TOK_SPLICE:
        for (int j = 1; j < bprm->argc; j++) {
            if (*n >= max)
                return -E2BIG;
            if (!bprm->args[j])
                continue;
            out[(*n)++] = bprm->args[j];
            bprm->args[j] = NULL;   /* ownership transferred */
        }
        return 0;
    default:
        return -EINVAL;
    }
}

/*
 * Give the translator the environment it was configured to need.
 *
 * Injected entries go to the *front* of envp.  Lookups take the first match,
 * so prepending is what makes an administrator-forced variable actually win
 * over a stray one the caller happened to export -- which is the whole point
 * of configuring it (ROSETTA_TMPDIR pointing somewhere writable is not
 * negotiable for a Rosetta-style translator).
 *
 * Called only after the argv rewrite has been installed, so a failure here
 * leaves the bprm in a consistent state the caller's bprm_free() can unwind.
 */
static int xlator_inject_env(exec_bprm_t *bprm, const char *spec,
                             const task_t *t)
{
    if (!spec || !spec[0])
        return 0;

    char scratch[XLATOR_ENV_LEN];
    size_t len = strlen(spec);
    if (len >= sizeof(scratch))
        return -EINVAL;
    memcpy(scratch, spec, len + 1);

    char *entries[XLATOR_ENV_MAX];
    int n = xlator_split_env(scratch, entries, XLATOR_ENV_MAX);
    if (n < 0)
        return n;   /* boot-validated; unreachable in practice */

    if (bprm->envc + n > MAX_ARG_STRINGS)
        return -E2BIG;

    /* Charge the injected strings against the same budget the caller's own
     * argv and envp were charged against, so a configured translator cannot
     * push the new image's initial stack past the limit. The budget check
     * itself is Rust-owned under the Rust selector. */
    size_t added = 0;
    for (int i = 0; i < n; i++)
        added += strlen(entries[i]) + 1;
    size_t budget = t->limits.stack / 4;
    long charged = a20_elf_check_arg_bytes(bprm->bytes, added, UINT64_MAX,
                                           MAX_ARG_BYTES, budget);
    if (charged < 0)
        return (int)charged;

    for (int i = 0; i < n; i++) {
        size_t elen = strlen(entries[i]) + 1;
        char *copy = kmalloc(elen);
        if (!copy)
            return -ENOMEM;
        memcpy(copy, entries[i], elen);
        /* Shift the existing environment up by one slot per entry already
         * placed, so the block ends in the right order with its NULL
         * terminator intact. */
        memmove(&bprm->envs[i + 1], &bprm->envs[i],
                (size_t)(bprm->envc - i + 1) * sizeof(char *));
        bprm->envs[i] = copy;
        bprm->envc++;
    }

    bprm->bytes = (size_t)charged;
    return 0;
}

static int exec_try_translator(int fd, exec_bprm_t *bprm, task_t *t)
{
    if (!xlator_enabled())
        return 0;

    /* What the file says it is -- architecture and ABI, both read out of the
     * bytes.  The ABI matters because it is not the same test as the
     * architecture: a foreign-ABI machine still needs a translator configured
     * for its *particular* ABI, or forwarding it would hand a native-ABI
     * image to a Linux-ABI translator that cannot load it. */
    elf_guest_key_t key;
    if (elf_probe_foreign(fd, &key) < 0)
        return 0;   /* not ours: corrupt file or native image */

    xlator_desc_t desc;
    if (xlator_lookup(&key, &desc) < 0)
        return 0;   /* channel on, but nothing usable for this guest */

    xlator_tmpl_t tmpl;
    if (xlator_parse_template(desc.argv, &tmpl) < 0)
        return 0;   /* boot-validated; unreachable without a torn config */

    kinfo("[XLATOR] pid=%d execve %s (e_machine=%u abi=%s) → %s  argv=\"%s\"\n",
          t->pid, bprm->path, key.machine, xlator_abi_name(key.abi),
          desc.path, desc.argv);

    /*
     * Build the new argv before touching bprm, so a failure part-way
     * through leaves the caller's bprm intact and the ENOEXEC path
     * recoverable.
     *
     * argv[0] is always the translator itself, whatever the template says:
     * the kernel is re-execing that program, and a template that tried to
     * place the guest image there would just be a confusing way to say so.
     */
    char *new_args[MAX_ARG_STRINGS + 1];
    int new_argc = 0;
    int r;

    r = xlator_push(new_args, &new_argc, MAX_ARG_STRINGS, desc.path);
    if (r < 0)
        goto fail;

    for (int i = 0; i < tmpl.n; i++) {
        r = xlator_push_token(&tmpl, i, bprm, new_args, &new_argc,
                              MAX_ARG_STRINGS);
        if (r < 0)
            goto fail;
    }
    new_args[new_argc] = NULL;

    /* bprm->path is now duplicated into new_args; release the original. */
    if (bprm->args[0]) { kfree(bprm->args[0]); bprm->args[0] = NULL; }
    if (bprm->path) { kfree(bprm->path); bprm->path = NULL; }

    memcpy(bprm->args, new_args, (new_argc + 1) * sizeof(char *));
    bprm->argc = new_argc;

    bprm->path = kmalloc(strlen(desc.path) + 1);
    if (!bprm->path)
        return -ENOMEM;   /* bprm is consistent; the caller frees it */
    strcpy(bprm->path, desc.path);

    r = xlator_inject_env(bprm, desc.env, t);
    if (r < 0)
        return r;

    /*
     * The image about to be loaded is the translator, not a guest, so it
     * will legitimately request an RWX JIT buffer.  Flag it here, before
     * the retry loop calls elf_load()/mmap, and let mm_wx_filter_prot()
     * honour exactly this one task.
     *
     * The kernel cannot know whether the configured translator actually JITs,
     * so a translator that does not is exempted too.  That is a deliberate
     * over-approximation: the alternative is a per-translator "needs W^X"
     * property in the configuration, which would be a permission decision
     * made from a string on a command line.  See
     * docs/exec-xlator/01-usage.md for the cost.
     */
    t->xlator_host = 1;

    return EXEC_RETRY;

fail:
    /* Covers both the allocation failure and the argument-count overflow.
     * Entries already spliced in from the bprm are in new_args and were
     * nulled at the source, so freeing the whole array frees each string
     * exactly once and the bprm stays internally consistent. */
    for (int i = 0; i < new_argc; i++)
        kfree(new_args[i]);
    return r;
}

#endif /* CONFIG_XLATOR */

/* ================================================================== */
/*  Native ABI setup                                                  */
/* ================================================================== */

#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
typedef struct {
    struct a20_ht_internal *table;
    bool created;
    a20_handle_t reserved[7];
    uint32_t reserved_count;
    uint32_t staged_count;
    struct {
        void *object;
        uint16_t type;
        a20_rights_t rights;
    } staged[7];
} exec_native_handles_t;

static int64_t exec_native_stage(exec_native_handles_t *state, void *object,
                                uint16_t type, a20_rights_t rights)
{
    if (!object || state->staged_count >= state->reserved_count) {
        return -A20_ERR_NO_SPACE;
    }
    uint32_t i = state->staged_count++;
    state->staged[i].object = object;
    state->staged[i].type = type;
    state->staged[i].rights = rights;
    return (int64_t)state->reserved[i];
}

static void exec_native_rollback(exec_native_handles_t *state)
{
    for (uint32_t i = 0; i < state->staged_count; i++) {
        if (state->staged[i].object)
            a20_object_release(state->staged[i].object, state->staged[i].type);
    }
    if (state->table && state->reserved_count)
        a20_handle_abort_reserved(state->table, state->reserved,
                                  state->reserved_count);
    if (state->created && state->table)
        a20_ht_put_ref(state->table);
    state->table = NULL;
    state->created = false;
    state->reserved_count = 0;
    state->staged_count = 0;
}

static int64_t exec_native_prepare_task_ref(task_t *task, void **object_out)
{
    struct a20_ht_internal *temporary = a20_ht_create();
    if (!temporary)
        return -A20_ERR_NO_MEMORY;
    int64_t handle = a20_handle_install_task(
        temporary, task, A20_OBJ_TASK,
        A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT | A20_RIGHT_DUP);
    if (handle >= 0) {
        a20_handle_entry_t entry;
        int64_t result = a20_handle_lookup_ref_internal(
            temporary, (a20_handle_t)handle, A20_OBJ_TASK, 0, &entry);
        if (result >= 0) {
            *object_out = entry.object; /* lookup_ref owns the staged ref */
            (void)a20_handle_remove(temporary, (a20_handle_t)handle);
            handle = A20_OK;
        } else {
            handle = result;
        }
    }
    a20_ht_put_ref(temporary);
    return handle;
}

static void exec_native_publish(task_t *task, exec_native_handles_t *state)
{
    for (uint32_t i = 0; i < state->staged_count; i++) {
        int64_t r = a20_handle_commit_reserved_temporal(
            state->table, state->reserved[i], state->staged[i].object,
            state->staged[i].type, state->staged[i].rights, UINT64_MAX, 0, 0, 0);
        if (r == A20_OK)
            state->staged[i].object = NULL; /* table owns the reference */
    }
    if (state->reserved_count > state->staged_count)
        a20_handle_abort_reserved(state->table,
                                  &state->reserved[state->staged_count],
                                  state->reserved_count - state->staged_count);
    if (state->created && state->table) {
        void *old = __atomic_exchange_n(&task->a20_ht, state->table, __ATOMIC_ACQ_REL);
        if (old && old != state->table)
            a20_ht_put_ref((struct a20_ht_internal *)old);
    }
    state->table = NULL;
    state->created = false;
    state->reserved_count = 0;
    state->staged_count = 0;
}

/*
 * Set up handle table, console handles, and a20_start_info_t on the
 * stack for a native ABI process.  Returns the new stack pointer, or 0
 * on failure.
 */
static uint64_t exec_setup_native_abi(task_t *t,
                                       const elf_load_info_t *info,
                                       int argc, char *const *k_argv,
                                       char *const *k_envp,
                                       vaddr_t *start_info_out,
                                       exec_native_handles_t *state)
{
    /* A native->native exec preserves the caller's handle table so that
     * fork+exec keeps its stdio (pipes/files the fork child wired up via
     * dup2).  A Linux->native exec builds a fresh table. */
    struct a20_ht_internal *ht = (struct a20_ht_internal *)t->a20_ht;
    int preserve = (ht != NULL);
    if (!ht) {
        ht = a20_ht_create();
        state->created = ht != NULL;
    }
    state->table = ht;
    if (ht && !(preserve && t->a20_stdio_adopted)) {
        if (a20_handle_reserve_many(ht, state->reserved,
                                    sizeof(state->reserved) / sizeof(state->reserved[0])) < 0)
            return 0;
        state->reserved_count = sizeof(state->reserved) / sizeof(state->reserved[0]);
    }

    uint32_t stdin_h = 0xFFFFFFFF, stdout_h = 0xFFFFFFFF, stderr_h = 0xFFFFFFFF;
    if (ht) {
        if (preserve && t->a20_stdio_adopted) {
            /* mlibc declared its current fd 0/1/2 handles (A20_SYS_task_adopt);
             * they are valid in the preserved table. */
            stdin_h = t->a20_stdin_h;
            stdout_h = t->a20_stdout_h;
            stderr_h = t->a20_stderr_h;
        } else {
            /* Inherit the process's real fds 0/1/2 into the native stdio
             * handles so that shell redirections (pipes/files) survive a
             * Linux->Native execve.  Fall back to /dev/console when an fd is
             * closed. */
            struct {
                int         fd;
                a20_rights_t rights;
                uint32_t   *out;
            } stdio[3] = {
                { 0, A20_RIGHT_READ  | A20_RIGHT_SEEK | A20_RIGHT_DUP | A20_RIGHT_CONTROL, &stdin_h },
                { 1, A20_RIGHT_WRITE | A20_RIGHT_SEEK | A20_RIGHT_DUP | A20_RIGHT_CONTROL, &stdout_h },
                { 2, A20_RIGHT_WRITE | A20_RIGHT_SEEK | A20_RIGHT_DUP | A20_RIGHT_CONTROL, &stderr_h },
            };
            for (int i = 0; i < 3; i++) {
                /* Each native stdio handle owns its own vfile reference; no
                 * fd slot is involved. */
                vfile_t *vf = fdtable_get_file_ref(t, stdio[i].fd, NULL);
                if (!vf) {
                    int flags = (stdio[i].rights & A20_RIGHT_READ) ? O_RDONLY : O_WRONLY;
                    int cfd = vfs_open("/dev/console", flags, 0);
                    if (cfd < 0)
                        continue;
                    vf = fdtable_get_current_file_ref(cfd);
                    fdtable_close_current(cfd);
                    if (!vf)
                        continue;
                }
                int64_t h = exec_native_stage(state, vf, A20_OBJ_FILE,
                                              stdio[i].rights);
                if (h >= 0) {
                    *stdio[i].out = (uint32_t)h;
                } else {
                    vfs_put_file(vf);
                }
            }
        }
    }

    a20_handle_t self_h = 0;
    if (ht) {
        if (preserve && t->a20_stdio_adopted) {
            self_h = t->a20_self_h;
        } else {
            void *task_object = NULL;
            int64_t h = exec_native_prepare_task_ref(t, &task_object);
            if (h >= 0)
                h = exec_native_stage(state, task_object, A20_OBJ_TASK,
                                      A20_RIGHT_WAIT | A20_RIGHT_SIGNAL |
                                          A20_RIGHT_STAT | A20_RIGHT_DUP);
            if (h < 0 && task_object)
                a20_object_release(task_object, A20_OBJ_TASK);
            if (h >= 0) self_h = (a20_handle_t)h;
        }
    }

    uint32_t root_h = 0, cwd_h = 0;
    if (ht) {
        if (preserve && t->a20_stdio_adopted) {
            root_h = t->a20_root_h;
            cwd_h = t->a20_cwd_h != A20_HANDLE_NULL ? t->a20_cwd_h : root_h;
        } else {
            int root_fd = vfs_open("/", O_RDONLY, 0);
            if (root_fd >= 0) {
                vfile_t *root_vf = fdtable_get_current_file_ref(root_fd);
                fdtable_close_current(root_fd); /* the handles own the refs */
                if (root_vf) {
                    /* Each installed handle owns one reference: install1
                     * takes over the lookup ref, install2 gets its own. */
                    int64_t h = exec_native_stage(state, root_vf, A20_OBJ_DIRECTORY,
                                                  A20_RIGHT_READ | A20_RIGHT_STAT |
                                                  A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
                    if (h >= 0) {
                        root_h = (uint32_t)h;
                        vfile_get(root_vf);
                        int64_t h2 = exec_native_stage(state, root_vf,
                                                       A20_OBJ_DIRECTORY,
                                                       A20_RIGHT_READ | A20_RIGHT_STAT |
                                                       A20_RIGHT_DUP);
                        if (h2 >= 0) cwd_h = (uint32_t)h2;
                        else vfs_put_file(root_vf);
                        if (!cwd_h) cwd_h = root_h;
                    } else {
                        vfs_put_file(root_vf);
                    }
                }
            }
        }
    }

    /* Well-known service registry client endpoint (M3). */
    uint32_t registry_h = 0;
    if (ht) {
        extern int64_t a20_registry_install_client(struct a20_ht_internal *ht);
        a20_channel_ep_t *ep = a20_registry_client_ep();
        if (ep) {
            a20_object_ref(ep, A20_OBJ_CHANNEL_ENDPOINT);
            int64_t h = exec_native_stage(state, ep, A20_OBJ_CHANNEL_ENDPOINT,
                                          A20_RIGHT_READ | A20_RIGHT_WRITE);
            if (h >= 0) registry_h = (uint32_t)h;
            else a20_object_release(ep, A20_OBJ_CHANNEL_ENDPOINT);
        }
    }

    if (info->interp_base) {
        return elf_setup_stack_a20_dynamic(
            info->stack_top, argc, k_argv, k_envp, info,
            stdin_h, stdout_h, stderr_h, self_h, root_h, cwd_h,
            0,
            start_info_out);
    }

    return elf_setup_stack_a20(info->stack_top, argc,
                               k_argv, k_envp, info,
                               stdin_h, stdout_h, stderr_h, self_h,
                               root_h, cwd_h, 0, registry_h);
}
#endif /* CONFIG_ABI_NATIVE */

/* ================================================================== */
/*  exec_install_process — swap mm, set up trap, signals, creds       */
/* ================================================================== */

/*
 * Atomically replaces the current process's address space and register
 * state with the newly loaded ELF image.  Handles both Linux and
 * Native ABI stack layouts.
 *
 * IMPORTANT: Prepares the new mm fully BEFORE detaching the old one,
 * so that failure during new-mm setup can be rolled back without
 * losing the old address space.
 *
 * @t       the current task
 * @info    ELF load result
 * @bprm    the exec parameters (args, path)
 * @abs_path resolved absolute path for t->exec_path
 * @st      stat result (may be NULL if stat failed)
 */
#ifdef CONFIG_ELF_EXEC_TEST
/*
 * One-shot exec fault injection for the test profile. Arming marks the
 * current task's next exec to fail at `phase`; only phases Rust marks
 * failable (a20_elf_phase_failable) may be armed, so the injection cannot
 * invent a failure stage the production transaction does not have.
 */
static struct {
    void *owner;
    unsigned phase;
} g_exec_test_fail;

int proc_exec_test_arm(const void *owner, unsigned phase)
{
    if (!owner || !a20_elf_phase_failable(phase))
        return -EINVAL;
    __atomic_store_n(&g_exec_test_fail.phase, phase, __ATOMIC_RELEASE);
    __atomic_store_n(&g_exec_test_fail.owner, (void *)owner, __ATOMIC_RELEASE);
    return 0;
}

static int exec_test_should_fail(const void *t, unsigned phase)
{
    if (__atomic_load_n(&g_exec_test_fail.owner, __ATOMIC_ACQUIRE) != t)
        return 0;
    if (__atomic_load_n(&g_exec_test_fail.phase, __ATOMIC_ACQUIRE) != phase)
        return 0;
    __atomic_store_n(&g_exec_test_fail.owner, NULL, __ATOMIC_RELEASE);
    return 1;
}
#endif /* CONFIG_ELF_EXEC_TEST */

typedef struct {
    task_t *task;
    const elf_load_info_t *info;
    const exec_bprm_t *bprm;
    const char *path;
    const kstat_t *stat;
    uint64_t sp;
    uintptr_t auxv[A20_AUXV_MAX_PAIRS][2];
    uint32_t auxv_n;
    vaddr_t ehdr_vdso;
    vaddr_t native_start_info;
    mm_struct_t *new_mm;
    mm_struct_t *old_mm;
    pt_root_t *old_pgdir;
    int old_abi;
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    exec_native_handles_t native_handles;
#endif
} exec_rust_context_t;

static int32_t exec_cb_linux_stack(void *opaque)
{
    exec_rust_context_t *c = opaque;
    c->ehdr_vdso = vdso_auxv_ehdr();
    c->sp = elf_setup_stack(c->info->stack_top, c->bprm->argc,
        (char *const *)c->bprm->args, (char *const *)c->bprm->envs,
        c->info, c->ehdr_vdso, c->auxv, &c->auxv_n);
    return c->sp ? 0 : -ENOMEM;
}

static int32_t exec_cb_native_stack(void *opaque)
{
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    exec_rust_context_t *c = opaque;
    c->sp = exec_setup_native_abi(c->task, c->info, c->bprm->argc,
        (char *const *)c->bprm->args, (char *const *)c->bprm->envs,
        &c->native_start_info, &c->native_handles);
    return c->sp ? 0 : -ENOMEM;
#else
    (void)opaque;
    return -ENOEXEC;
#endif
}

static int32_t exec_cb_vdso(void *opaque)
{
    exec_rust_context_t *c = opaque;
    if (c->info->is_native_abi || !c->sp || !c->ehdr_vdso)
        return 0;
    return vdso_map_image(c->info->pgdir, (mm_seg_t **)&c->info->mmap) < 0
        ? -ENOMEM : 0;
}

static int32_t exec_cb_get_abi(void *opaque)
{ return ((exec_rust_context_t *)opaque)->task->abi_mode; }

static void exec_cb_set_abi(void *opaque, int32_t mode)
{ ((exec_rust_context_t *)opaque)->task->abi_mode = mode; }

static int32_t exec_cb_alloc_mm(void *opaque, uint64_t *out)
{
    exec_rust_context_t *c = opaque;
    mm_struct_t *m = kcalloc(1, sizeof(*m));
    if (!m) return -ENOMEM;
    m->pgdir = c->info->pgdir;
    m->dumpable = 1;
    m->brk = m->start_brk = c->info->brk;
    m->mmap_base = mm_aslr_mmap_base();
    m->stack_top = c->info->stack_top;
    m->stack_bottom = c->info->stack_top - USER_STACK_INITIAL_PAGES * PAGE_SIZE;
    mm_rss_set(m, 0);
    spin_init(&m->lock); spin_set_debug(&m->lock, "mm", m);
    mutex_init(&m->tlb_lock); m->tlb_holds = NULL;
    refcount_set(&m->refcount, 1);
    m->mmap = c->info->mmap;
    m->has_vdso = vdso_auxv_ehdr() != 0;
#ifdef CONFIG_NOMMU
    m->num_nommu_allocs = c->info->num_nommu_allocs;
    for (int i = 0; i < c->info->num_nommu_allocs && i < NOMMU_ALLOC_MAX; i++) {
        m->nommu_allocs[i] = c->info->nommu_allocs[i];
        m->nommu_alloc_sizes[i] = c->info->nommu_alloc_sizes[i];
        m->nommu_alloc_types[i] = c->info->nommu_alloc_types[i];
    }
#endif
    arch_setup_signal_trampoline(m);
    c->new_mm = m;
    *out = (uint64_t)(uintptr_t)m;
    return 0;
}

static void exec_cb_auxv(void *opaque)
{
    exec_rust_context_t *c = opaque;
    c->new_mm->auxv_n = c->auxv_n;
    for (uint32_t i = 0; i < c->auxv_n && i < A20_AUXV_MAX_PAIRS; i++) {
        c->new_mm->auxv[i][0] = c->auxv[i][0];
        c->new_mm->auxv[i][1] = c->auxv[i][1];
    }
}

static int32_t exec_cb_unshare(void *opaque)
{ return fdtable_unshare(((exec_rust_context_t *)opaque)->task); }
static void exec_cb_siblings(void *opaque)
{
    exec_rust_context_t *c = opaque;
    /* Once this returns, no sibling can observe staged slots in an existing
     * shared table. Publication and the remaining exec steps cannot fail. */
    proc_exec_terminate_siblings(c->task);
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    exec_native_publish(c->task, &c->native_handles);
#endif
}
static void exec_cb_cloexec(void *opaque)
{ fdtable_close_on_exec(((exec_rust_context_t *)opaque)->task); }

static void exec_cb_trap(void *opaque)
{
    exec_rust_context_t *c = opaque;
    task_t *t = c->task;
    const elf_load_info_t *info = c->info;
    mm_arch_context_init(c->new_mm);
    uint64_t user_as = mm_address_space_token(c->new_mm);
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    c->old_mm = t->mm; c->old_pgdir = t->pgdir;
    t->mm = c->new_mm; t->pgdir = info->pgdir;
    spin_unlock_irqrestore(&t->park_lock, flags);
    t->entry = info->entry; t->ustack = c->sp;
    strncpy(t->exec_path, c->path, MAX_PATH_LEN - 1);
    t->exec_path[MAX_PATH_LEN - 1] = '\0';
    const char *base = strrchr(t->exec_path, '/');
    proc_set_name(t, base ? base + 1 : t->exec_path);
    signal_exec_reset(t);
    uint64_t ks_top = (uint64_t)(uintptr_t)t->kstack_base + KERNEL_STACK_SIZE;
    if (!t->trap_ctx) {
        trap_context_t *trap = (trap_context_t *)(uintptr_t)(ks_top - sizeof(*trap));
        task_context_t *ctx = arch_task_context_base(t->kstack_base, ks_top, trap);
        memset(ctx, 0, sizeof(*ctx));
        t->first_kernel_entry = (uintptr_t)user_trap_return;
        ctx->ra = (uint64_t)(uintptr_t)proc_task_first_entry;
        ctx->tp = (uint64_t)(uintptr_t)t;
        arch_task_context_set_user_tp(ctx, info->tls_tp);
        TASK_CTX_STATUS(ctx) = arch_task_user_resume_status();
        TASK_CTX_PAGE_TABLE(ctx) = user_as;
        arch_task_context_set_initial_sp(ctx, trap, ks_top);
        t->trap_ctx = trap; t->kstack = (uint64_t)(uintptr_t)ctx;
    }
    trap_context_t *trap = t->trap_ctx;
    ks_top = arch_trap_ctx_get_kernel_stack(trap, (uint64_t)(uintptr_t)trap);
    memset(trap, 0, sizeof(*trap));
    TRAP_CTX_KScratch0(trap) = user_as;
    arch_trap_ctx_set_user_entry(trap, info->entry);
    TRAP_CTX_SP(trap) = c->sp; TRAP_CTX_TP(trap) = info->tls_tp;
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    if (info->is_native_abi)
        TRAP_CTX_SET_ARG0(trap, c->native_start_info ? c->native_start_info : c->sp);
#endif
    trap->kernel_tp = (uint64_t)(uintptr_t)t;
    arch_trap_ctx_set_kernel_stack(trap, ks_top);
    TRAP_CTX_STATUS(trap) = arch_user_initial_status();
}

static void exec_cb_creds(void *opaque)
{
    exec_rust_context_t *c = opaque;
    c->task->exec_load_addr = c->info->load_addr;
    c->task->exec_load_size = c->info->load_size;
    if (c->stat) proc_apply_exec_creds(c->task, c->stat);
}

static void exec_cb_enter(void *opaque)
{
    exec_rust_context_t *c = opaque;
    task_t *t = c->task;
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    if (c->old_abi == 1) {
        /* A Native->Native exec reuses the table whose handles were written
         * into its new start-info. Only Native->Linux retires it here. */
        if (!c->info->is_native_abi) {
            void *old = __atomic_exchange_n(&t->a20_ht, NULL, __ATOMIC_ACQ_REL);
            if (old) a20_ht_put_ref((struct a20_ht_internal *)old);
        }
    }
#endif
    unsigned cpu = cpu_current_id();
    uint64_t as = mm_address_space_token(c->new_mm);
    mm_context_enter(c->new_mm, cpu); arch_switch_addr_space_token(as);
    if (c->old_mm && c->old_mm != c->new_mm) mm_context_leave(c->old_mm, cpu);
    if (c->old_mm) mm_destroy(c->old_mm);
    else if (c->old_pgdir && c->old_pgdir != proc_kernel_pgdir_shared()) pt_destroy_user(c->old_pgdir);
    proc_complete_vfork(t);
#ifdef CONFIG_NOMMU
    arch_flush_icache_range((const void *)c->info->load_addr, c->info->load_size);
#else
    arch_fence_i();
#endif
}

static void exec_cb_undo(void *opaque, uint32_t kind, uint64_t handle)
{
    exec_rust_context_t *c = opaque;
    if (kind == A20_ELF_RES_NEW_MM) { kfree((void *)(uintptr_t)handle); c->new_mm = NULL; }
    else if (kind == A20_ELF_RES_ABI_MODE) c->task->abi_mode = (int)handle;
    else if (kind == A20_ELF_RES_IMAGE) {
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
        exec_native_rollback(&c->native_handles);
#endif
        /* handle is the 64-bit resource slot; go through uintptr_t so a
         * 32-bit port truncates to the pointer width instead of failing the
         * int-to-pointer-cast check. */
        elf_load_info_discard((elf_load_info_t *)(uintptr_t)handle);
    }
}

#ifdef CONFIG_ELF_EXEC_TEST
static bool exec_cb_inject(void *opaque, uint32_t phase, bool after)
{
    /* The high bit denotes an after-effect probe. Existing scripts keep the
     * historical before-effect encoding. */
    exec_rust_context_t *c = opaque;
    return exec_test_should_fail(c->task, phase | (after ? 0x100u : 0));
}
#endif

/*
 * Transaction-driven image replacement.
 *
 * The phase order still comes from elf_core (`COMMIT_PHASES`, walked through
 * a20_elf_txn_begin) because the sequence itself is a decision; the *resource
 * accounting* now comes from the same Rust transaction object. Every phase
 * that acquires something the adapter would have to free registers it with
 * a20_elf_txn_acquire, and a failure releases exactly the live set the
 * transaction still holds, newest first — not a static "undo these phases"
 * list. a20_elf_txn_commit refuses to run until every phase has completed, so
 * the commit point cannot hide a half-run setup.
 *
 * Rust starts the irreversible region before sibling termination. No phase
 * after fdtable unshare can return an error; the remaining callbacks tear down
 * siblings/descriptors, install the new context and enter the new image.
 */
static int exec_install_process(task_t *t,
                                 const elf_load_info_t *info,
                                 const exec_bprm_t *bprm,
                                 const char *abs_path,
                                 const kstat_t *st)
{
    exec_rust_context_t c = {
        .task = t, .info = info, .bprm = bprm, .path = abs_path,
        .stat = st, .old_abi = t->abi_mode,
    };
    const a20_elf_exec_ops_t ops = {
        .build_linux_stack = exec_cb_linux_stack,
        .build_native_stack = exec_cb_native_stack,
        .map_vdso = exec_cb_vdso,
        .get_abi_mode = exec_cb_get_abi,
        .set_abi_mode = exec_cb_set_abi,
        .allocate_new_mm = exec_cb_alloc_mm,
        .publish_auxv = exec_cb_auxv,
        .unshare_fdtable = exec_cb_unshare,
        .terminate_siblings = exec_cb_siblings,
        .close_on_exec = exec_cb_cloexec,
        .install_trap_context = exec_cb_trap,
        .apply_credentials = exec_cb_creds,
        .enter_new_mm = exec_cb_enter,
        .undo_resource = exec_cb_undo,
#ifdef CONFIG_ELF_EXEC_TEST
        .inject_failure = exec_cb_inject,
#else
        .inject_failure = NULL,
#endif
    };
    return a20_elf_exec_run((uint64_t)(uintptr_t)info,
                            info->is_native_abi, &c, &ops);
}

/* ================================================================== */
/*
 * PT_DEBUG_EXEC_STOP: a traced task stops right after a successful exec.
 * PTRACE_TRACEME children always stop here (classic SIGTRAP exec stop, which
 * is how a tracer learns the tracee's new image); with the TRACEEXEC option
 * the stop is reported as a PTRACE_EVENT_EXEC watch event instead.  The
 * stop runs in tracee context and blocks until the tracer resumes the task.
 */
static void exec_trace_stop(task_t *t)
{
    if (!t || !proc_debug_is_traced(t))
        return;
    if (t->ptrace_flags & PT_DEBUG_FLAG_TRACEEXEC)
        (void)proc_debug_event_stop(SIGTRAP, PT_DEBUG_EVENT_EXEC, 0);
    else if (t->ptrace_flags & PT_DEBUG_FLAG_TRACEME)
        (void)proc_debug_signal_stop(SIGTRAP);
}

/*  proc_exec — public API                                            */
/* ================================================================== */

int proc_exec(const char *path, char *const argv[], char *const envp[])
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;

    /* ---- 1. Initialise bprm and copy args from user ONCE ---- */
    exec_bprm_t bprm;
    memset(&bprm, 0, sizeof(bprm));

    bprm.path = kmalloc(strlen(path) + 1);
    if (!bprm.path)
        return -ENOMEM;
    strcpy(bprm.path, path);

    size_t arg_bytes = 0;
    int r = exec_copy_args(argv, bprm.args, &bprm.argc,
                           &arg_bytes, MAX_ARG_BYTES);
    if (r < 0) {
        kfree(bprm.path);
        return r;
    }
    bprm.bytes = arg_bytes;
    /*
     * Linux accepts execve(path, NULL, envp), and also an argv array whose
     * first entry is NULL, by supplying an empty argv[0].  Modern glibc
     * rejects a literal argc == 0 during startup, so normalize the call here
     * before constructing the initial stack.  Use the requested path as the
     * useful argv[0] value while preserving the Linux guarantee argc >= 1.
     */
    if (bprm.argc == 0) {
        size_t len = strlen(bprm.path) + 1;
        long next0 = a20_elf_check_arg_bytes(arg_bytes, len, MAX_ARG_STRLEN,
                                             MAX_ARG_BYTES,
                                             t ? (uint64_t)(t->limits.stack / 4) : 0);
        if (next0 < 0) {
            bprm_free(&bprm);
            return (int)next0;
        }
        bprm.args[0] = kmalloc(len);
        if (!bprm.args[0]) {
            bprm_free(&bprm);
            return -ENOMEM;
        }
        memcpy(bprm.args[0], bprm.path, len);
        bprm.args[1] = NULL;
        bprm.argc = 1;
        arg_bytes = (size_t)next0;
        bprm.bytes = arg_bytes;
    }

    r = exec_copy_args(envp, bprm.envs, &bprm.envc,
                       &arg_bytes, MAX_ARG_BYTES);
    bprm.bytes = arg_bytes;
    if (r < 0) {
        bprm_free(&bprm);
        return r;
    }

    /* ---- 2. Resolve absolute path for exec_path ---- */
    char abs_path[MAX_PATH_LEN];
    const char *cwd = t->fs.cwd[0] ? t->fs.cwd : "/";
    const char *root = t->fs.root_path[0] ? t->fs.root_path : "/";
    if (bprm.path[0] == '/') {
        if (strcmp(root, "/") == 0) {
            strncpy(abs_path, bprm.path, MAX_PATH_LEN - 1);
            abs_path[MAX_PATH_LEN - 1] = '\0';
        } else {
            snprintf(abs_path, MAX_PATH_LEN, "%s%s", root, bprm.path);
        }
    } else {
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] == '/')
            snprintf(abs_path, MAX_PATH_LEN, "%s%s", cwd, bprm.path);
        else
            snprintf(abs_path, MAX_PATH_LEN, "%s/%s", cwd, bprm.path);
        if (strcmp(root, "/") != 0) {
            char rooted[MAX_PATH_LEN];
            snprintf(rooted, MAX_PATH_LEN, "%s%s", root, abs_path);
            strncpy(abs_path, rooted, MAX_PATH_LEN - 1);
            abs_path[MAX_PATH_LEN - 1] = '\0';
        }
    }
    vfs_path_normalize_absolute_with_root(abs_path, root);
    if (bprm.path[0] != '/') {
        r = exec_replace_path(&bprm, abs_path);
        if (r < 0) {
            bprm_free(&bprm);
            return r;
        }
    }

    /* ---- 3. Iterative format resolution loop ---- */
    kstat_t exec_st;

    for (bprm.depth = 0; bprm.depth < EXEC_MAX_DEPTH; bprm.depth++) {
        /* Open and validate the file */
        int fd = exec_open_and_check(bprm.path, &exec_st, abs_path);
        if (fd < 0) {
            bprm_free(&bprm);
            return fd;
        }

        /* Try ELF first */
        elf_load_info_t info;
        memset(&info, 0, sizeof(info));
        r = elf_load(fd, abs_path, &info);

        int outcome = a20_elf_classify(r);
        if (outcome == A20_ELF_STEP_INSTALL) {
            /* Keep metadata from the opened ELF. A pathname stat here could
             * select a replacement inode and apply its set-id credentials. */
            vfs_close(fd);

            r = exec_install_process(t, &info, &bprm, abs_path,
                                      &exec_st);
            bprm_free_strings(&bprm);
            kfree(bprm.path);
            if (r == 0)
                exec_trace_stop(t);
            return r;
        }

        /* ELF failed — try shebang, then the foreign-arch translator */
        if (outcome == 1) {   /* format mismatch */
            int sr = exec_try_script(fd, &bprm);
            switch (a20_elf_step_after_script(sr)) {
            case A20_ELF_STEP_RETRY:
                vfs_close(fd);
                continue;   /* loop with new interpreter */
            case A20_ELF_STEP_FAIL:
                vfs_close(fd);
                bprm_free(&bprm);
                return sr;
            default:   /* CONTINUE: not a script */
                break;
            }
#ifdef CONFIG_XLATOR
            int xr = exec_try_translator(fd, &bprm, t);
            vfs_close(fd);
            switch (a20_elf_step_after_xlator(true, xr)) {
            case A20_ELF_STEP_RETRY:
                xlator_note_forward();
                continue;   /* loop with the translator as the image */
            case A20_ELF_STEP_FAIL:
                bprm_free(&bprm);
                return xr;
            default:
                break;
            }
#endif
            bprm_free(&bprm);
            return -ENOEXEC;
        }

        /* ELF failed with a real error (not format mismatch) */
        vfs_close(fd);
        bprm_free(&bprm);
        return r;
    }

    /* Too many shebang levels */
    bprm_free(&bprm);
    return -ELOOP;
}
