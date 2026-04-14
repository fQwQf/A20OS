/*
 * A20OS — User-mode Interactive Shell (sh)
 *
 * Features:
 *   - Tab completion (partial)
 *   - Command history (up/down arrows)
 *   - Pipeline support: cmd1 | cmd2
 *   - I/O redirection: > >> <
 *   - Background jobs: cmd &
 *   - Environment variables: export VAR=val, $VAR
 *   - Aliases: alias, unalias
 *   - Conditional: && ||
 *   - Built-in commands: cd, pwd, exit, export, alias, echo, set, ...
 *
 * Delegates all other commands to execve (via PATH lookup).
 */

#include "../lib/libc.h"

/* ============================================================
 * Configuration
 * ============================================================ */
#define MAX_LINE        1024
#define MAX_ARGS        128
#define MAX_HIST        64
#define MAX_ENV         64
#define MAX_ALIASES     32

/* ============================================================
 * State
 * ============================================================ */

static char *history[MAX_HIST];
static int   hist_len  = 0;
static int   hist_idx  = 0;
static int   last_exit = 0;

/* Environment */
static char  env_store[MAX_ENV][256];
static int   env_count = 0;

/* Aliases */
typedef struct { char name[64]; char val[256]; } alias_t;
static alias_t g_aliases[MAX_ALIASES];
static int     g_nalias = 0;

/* ============================================================
 * Readline with history / tab-completion
 * ============================================================ */

static void hist_add(const char *line) {
    if (!line || !line[0]) return;
    /* Avoid duplicates */
    if (hist_len > 0 && strcmp(history[hist_len - 1], line) == 0) return;

    char *dup = strdup(line);
    if (!dup) return;
    if (hist_len < MAX_HIST) {
        history[hist_len++] = dup;
    } else {
        free(history[0]);
        for (int i = 0; i < MAX_HIST - 1; i++) history[i] = history[i + 1];
        history[MAX_HIST - 1] = dup;
    }
    hist_idx = hist_len;
}

static void clear_line(int len) {
    for (int i = 0; i < len; i++) {
        write(1, "\b \b", 3);
    }
}

/* Redraw buf[cursor..len-1], clear to EOL, reposition cursor back */
static void refresh_tail(char *buf, int cursor, int len) {
    int tail = len - cursor;
    if (tail > 0)
        write(1, &buf[cursor], tail);
    write(1, "\033[K", 3);          /* clear to end of line */
    if (tail > 0) {
        char esc[16];
        snprintf(esc, sizeof(esc), "\033[%dD", tail);
        write(1, esc, strlen(esc));
    }
}

/* Forward declaration — print_prompt() defined later */
static void print_prompt(void);

/* ============================================================
 * Tab auto-completion
 * ============================================================ */

#define TC_MAX 128

static void tab_complete(char *buf, int *pos, int *len, size_t sz) {
    int p = *pos, l = *len;

    /* ---- Locate the word being typed ---- */
    int ws = p;                              /* word start */
    while (ws > 0 && buf[ws - 1] != ' ') ws--;

    /* First word? (everything before ws is whitespace) */
    int first_word = 1;
    for (int i = 0; i < ws; i++)
        if (buf[i] != ' ') { first_word = 0; break; }

    int prefix_len = p - ws;                 /* chars already typed */

    int has_slash = 0;
    for (int i = ws; i < p; i++)
        if (buf[i] == '/') { has_slash = 1; break; }

    int is_command = first_word && !has_slash;

    /* ---- For file/path words: split into dir + filename prefix ---- */
    char dirpath[512];
    char fpbuf[256];
    int  fp_len = 0;

    if (!is_command) {
        int last_slash = -1;
        for (int i = ws; i < p; i++)
            if (buf[i] == '/') last_slash = i;

        if (last_slash >= 0) {
            if (buf[ws] == '/') {            /* absolute */
                int dlen = last_slash - ws;
                if (dlen == 0) { dirpath[0] = '/'; dirpath[1] = '\0'; }
                else { memcpy(dirpath, &buf[ws], dlen); dirpath[dlen] = '\0'; }
            } else {                         /* relative with '/' */
                getcwd(dirpath, sizeof(dirpath));
                int cl = (int)strlen(dirpath);
                if (cl > 0 && dirpath[cl - 1] != '/')
                    dirpath[cl++] = '/';
                memcpy(dirpath + cl, &buf[ws], last_slash - ws);
                dirpath[cl + last_slash - ws] = '\0';
            }
            fp_len = p - last_slash - 1;
            if (fp_len > 0) memcpy(fpbuf, &buf[last_slash + 1], fp_len);
            fpbuf[fp_len] = '\0';
        } else {
            getcwd(dirpath, sizeof(dirpath));
            fp_len = prefix_len;
            if (fp_len > 0) memcpy(fpbuf, &buf[ws], fp_len);
            fpbuf[fp_len] = '\0';
        }
    }

    /* ---- Collect matching candidates ---- */
    static char matches[TC_MAX][256];
    static int  is_dir[TC_MAX];
    int nmatch = 0;

    if (is_command) {
        /* Built-in commands */
        static const char *bnames[] = {
            "cd","exit","quit","export",
            "alias","unalias","history","type", NULL
        };
        for (int i = 0; bnames[i] && nmatch < TC_MAX; i++)
            if (strncmp(bnames[i], &buf[ws], prefix_len) == 0)
                strcpy(matches[nmatch++], bnames[i]);

        /* Executables in PATH */
        char *pe = getenv("PATH");
        if (!pe) pe = "/bin";
        char pbuf[512];
        strncpy(pbuf, pe, sizeof(pbuf) - 1);
        pbuf[sizeof(pbuf) - 1] = '\0';
        char *pp = pbuf;
        while (*pp && nmatch < TC_MAX) {
            char *end = pp;
            while (*end && *end != ':') end++;
            char sv = *end; *end = '\0';
            DIR *d = opendir(pp);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL && nmatch < TC_MAX) {
                    if (de->d_name[0] == '.') continue;
                    if (strncmp(de->d_name, &buf[ws], prefix_len) != 0) continue;
                    int dup = 0;
                    for (int k = 0; k < nmatch; k++)
                        if (strcmp(matches[k], de->d_name) == 0) { dup = 1; break; }
                    if (!dup) strcpy(matches[nmatch++], de->d_name);
                }
                closedir(d);
            }
            *end = sv;
            pp = end;
            if (sv == ':') pp++;
        }
    } else {
        /* Files in the relevant directory */
        DIR *d = opendir(dirpath);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL && nmatch < TC_MAX) {
                if (de->d_name[0] == '.' && (fp_len == 0 || fpbuf[0] != '.')) continue;
                if (strncmp(de->d_name, fpbuf, fp_len) != 0) continue;
                strcpy(matches[nmatch], de->d_name);
                is_dir[nmatch] = (de->d_type == DT_DIR);
                if (is_dir[nmatch]) strcat(matches[nmatch], "/");
                nmatch++;
            }
            closedir(d);
        }
    }

    if (nmatch == 0) return;

    int ep_len = is_command ? prefix_len : fp_len;   /* effective prefix */

    if (nmatch == 1) {
        /* ---- Single match: insert completion suffix ---- */
        char *suffix = matches[0] + ep_len;
        int slen = (int)strlen(suffix);
        int add_sp = first_word ? 1 : (slen == 0 || suffix[slen - 1] != '/');
        int total = slen + (add_sp ? 1 : 0);
        if (l + total >= (int)sz - 1) return;

        memmove(&buf[p + total], &buf[p], l - p);
        memcpy(&buf[p], suffix, slen);
        if (add_sp) buf[p + slen] = ' ';
        l += total; p += total; buf[l] = '\0';

        write(1, suffix, slen);
        if (add_sp) write(1, " ", 1);
        refresh_tail(buf, p, l);
    } else {
        /* ---- Multiple matches: partial complete, then list if stuck ---- */
        int clen = (int)strlen(matches[0]);
        for (int i = 1; i < nmatch && clen > 0; i++) {
            int j = 0;
            while (j < clen && matches[i][j] == matches[0][j]) j++;
            clen = j;
        }
        if (clen > ep_len) {
            /* Common prefix extends beyond what's typed — just complete,
             * don't show the list yet (user needs another Tab for that). */
            int extra = clen - ep_len;
            if (l + extra < (int)sz - 1) {
                memmove(&buf[p + extra], &buf[p], l - p);
                memcpy(&buf[p], matches[0] + ep_len, extra);
                l += extra; p += extra; buf[l] = '\0';
                write(1, matches[0] + ep_len, extra);
                refresh_tail(buf, p, l);
            }
        } else {
            /* Already at the longest common prefix — show all matches */
            write(1, "\n", 1);
            for (int i = 0; i < nmatch; i++) {
                int mlen = (int)strlen(matches[i]);
                if (is_dir[i]) {
                    write(1, "\033[1;34m", 7);
                    write(1, matches[i], mlen - 1);
                    write(1, "\033[0m", 4);
                    write(1, "/", 1);
                } else {
                    write(1, matches[i], mlen);
                }
                write(1, "  ", 2);
            }
            write(1, "\n", 1);
            print_prompt();
            write(1, buf, l);
            int back = l - p;
            if (back > 0) {
                char esc[16];
                snprintf(esc, sizeof(esc), "\033[%dD", back);
                write(1, esc, strlen(esc));
            }
        }
    }

    *pos = p;
    *len = l;
}

/* ============================================================
 * Main readline — insert mode + Tab completion
 * ============================================================ */

static int readline_with_history(char *buf, size_t sz) {
    int pos = 0;       /* cursor position within buf */
    int len = 0;       /* total characters in buf    */
    hist_idx = hist_len;

    while (1) {
        int c = getchar();
        if (c < 0) break;

        if (c == '\n' || c == '\r') {
            write(1, "\n", 1);
            buf[len] = '\0';
            return len;
        }

        if (c == 127 || c == '\b') {            /* Backspace */
            if (pos > 0) {
                memmove(&buf[pos - 1], &buf[pos], len - pos);
                pos--; len--;
                buf[len] = '\0';
                write(1, "\b", 1);
                refresh_tail(buf, pos, len);
            }
            continue;
        }

        if (c == 4) {                            /* Ctrl+D */
            if (len == 0) { write(1, "\n", 1); buf[0] = '\0'; return -1; }
            continue;
        }

        if (c == 3) {                            /* Ctrl+C */
            write(1, "^C\n", 3);
            buf[0] = '\0';
            return 0;
        }

        if (c == 12) {                           /* Ctrl+L: clear screen */
            write(1, "\033[2J\033[H", 7);
            print_prompt();
            write(1, buf, len);
            int back = len - pos;
            if (back > 0) {
                char esc[16];
                snprintf(esc, sizeof(esc), "\033[%dD", back);
                write(1, esc, strlen(esc));
            }
            continue;
        }

        if (c == '\t') {                         /* Tab completion */
            tab_complete(buf, &pos, &len, sz);
            continue;
        }

        if (c == 27) {                           /* Escape sequence */
            int c2 = getchar();
            if (c2 == '[') {
                int c3 = getchar();
                if (c3 == 'A') {                 /* Up arrow */
                    if (hist_idx > 0) {
                        clear_line(len);
                        hist_idx--;
                        strncpy(buf, history[hist_idx], sz - 1);
                        buf[sz - 1] = '\0';
                        len = (int)strlen(buf);
                        pos = len;
                        write(1, buf, len);
                    }
                } else if (c3 == 'B') {          /* Down arrow */
                    clear_line(len);
                    if (hist_idx < hist_len - 1) {
                        hist_idx++;
                        strncpy(buf, history[hist_idx], sz - 1);
                    } else {
                        hist_idx = hist_len;
                        buf[0] = '\0';
                    }
                    len = (int)strlen(buf);
                    pos = len;
                    write(1, buf, len);
                } else if (c3 == 'C') {          /* Right arrow */
                    if (pos < len) {
                        pos++;
                        write(1, "\033[C", 3);
                    }
                } else if (c3 == 'D') {          /* Left arrow */
                    if (pos > 0) {
                        pos--;
                        write(1, "\b", 1);
                    }
                } else if (c3 == '3') {          /* Delete: ESC [ 3 ~ */
                    int c4 = getchar();
                    if (c4 == '~' && pos < len) {
                        memmove(&buf[pos], &buf[pos + 1], len - pos - 1);
                        len--;
                        buf[len] = '\0';
                        refresh_tail(buf, pos, len);
                    }
                } else if (c3 == 'H') {          /* Home */
                    while (pos > 0) { pos--; write(1, "\b", 1); }
                } else if (c3 == 'F') {          /* End */
                    while (pos < len) {
                        write(1, &buf[pos], 1);
                        pos++;
                    }
                }
            }
            continue;
        }

        if ((unsigned char)c < 32) continue;     /* ignore other control */

        /* ---- Printable character: insert at cursor ---- */
        if ((size_t)(len + 1) < sz) {
            memmove(&buf[pos + 1], &buf[pos], len - pos);
            buf[pos] = (char)c;
            pos++; len++;
            buf[len] = '\0';
            /* Echo: write the new char + shifted tail */
            write(1, &buf[pos - 1], len - pos + 1);
            /* Move cursor back to pos */
            int back = len - pos;
            if (back > 0) {
                char esc[16];
                snprintf(esc, sizeof(esc), "\033[%dD", back);
                write(1, esc, strlen(esc));
            }
        }
    }
    buf[len] = '\0';
    return len;
}

/* ============================================================
 * Tokenize / Parse
 * ============================================================ */

/* Expand $VAR and $? in token */
static void expand_var(char *out, size_t osz, const char *in) {
    size_t i = 0, j = 0;
    while (in[i] && j < osz - 1) {
        if (in[i] == '$') {
            i++;
            if (in[i] == '?') {
                j += snprintf(out + j, osz - j, "%d", last_exit);
                i++;
            } else if (in[i] == '$') {
                j += snprintf(out + j, osz - j, "%d", getpid());
                i++;
            } else {
                char var[64]; int vl = 0;
                while (in[i] && (in[i] == '_' || (in[i] >= 'A' && in[i] <= 'Z')
                                 || (in[i] >= 'a' && in[i] <= 'z')
                                 || (in[i] >= '0' && in[i] <= '9')))
                    var[vl++] = in[i++];
                var[vl] = '\0';
                char *v = getenv(var);
                if (v) { size_t vl2 = strlen(v); if (j + vl2 < osz - 1) { memcpy(out + j, v, vl2); j += vl2; } }
            }
        } else {
            out[j++] = in[i++];
        }
    }
    out[j] = '\0';
}

/* Tokenize line into argv array. Handles quotes. */
static int tokenize(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    static char tokbuf[MAX_LINE];
    char *tok = tokbuf;

    while (*p && argc < max_args - 1) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break; /* Comment or end */
        if (*p == '|' || *p == '>' || *p == '<' || *p == '&' || *p == ';') {
            /* Special token */
            char special[3] = { *p, 0 };
            if (p[0] == '>' && p[1] == '>') { special[1] = '>'; special[2] = 0; p++; }
            else if (p[0] == '&' && p[1] == '&') { special[1] = '&'; special[2] = 0; p++; }
            else if (p[0] == '|' && p[1] == '|') { special[1] = '|'; special[2] = 0; p++; }
            argv[argc++] = strdup(special);
            p++;
            continue;
        }

        /* Regular token or quoted string */
        char quote = 0;
        if (*p == '"' || *p == '\'') { quote = *p++; }
        tok = tokbuf + (argc * 128 % (MAX_LINE - 128));
        int tl = 0;
        while (*p && (quote ? (*p != quote) : (*p != ' ' && *p != '\t'
               && *p != '|' && *p != '>' && *p != '<' && *p != '&' && *p != ';'))) {
            if (*p == '\\' && !quote) { p++; if (*p) tok[tl++] = *p++; continue; }
            tok[tl++] = *p++;
        }
        tok[tl] = '\0';
        if (quote && *p == quote) p++;

        /* Variable expansion */
        char expanded[256];
        expand_var(expanded, sizeof(expanded), tok);
        argv[argc++] = strdup(expanded);
    }
    argv[argc] = NULL;
    return argc;
}

/* ============================================================
 * Alias lookup
 * ============================================================ */

static const char *alias_lookup(const char *name) {
    for (int i = 0; i < g_nalias; i++)
        if (strcmp(g_aliases[i].name, name) == 0)
            return g_aliases[i].val;
    return NULL;
}

/* ============================================================
 * Find executable in PATH
 * ============================================================ */

static int find_in_path(const char *cmd, char *out, size_t osz) {
    if (cmd[0] == '/' || cmd[0] == '.') {
        strncpy(out, cmd, osz - 1);
        return access(out, 0) == 0 ? 0 : -1;
    }
    char *path = getenv("PATH");
    if (!path) path = "/bin";
    char pathbuf[512];
    strncpy(pathbuf, path, sizeof(pathbuf) - 1);
    char *tok = strtok(pathbuf, ":");
    while (tok) {
        snprintf(out, osz, "%s/%s", tok, cmd);
        if (access(out, 0) == 0) return 0;
        tok = strtok(NULL, ":");
    }
    return -1;
}

/* ============================================================
 * Built-in commands (must run in shell process)
 * ============================================================ */

static int builtin_cd(int argc, char *argv[]) {
    const char *dir = argc > 1 ? argv[1] : "/";
    if (chdir(dir) < 0) { printf("cd: %s: No such directory\n", dir); return 1; }
    return 0;
}

static int builtin_export(int argc, char *argv[]) {
    if (argc == 1) {
        for (int i = 0; environ && environ[i]; i++)
            printf("export %s\n", environ[i]);
        for (int i = 0; i < env_count; i++)
            printf("export %s\n", env_store[i]);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        if (env_count < MAX_ENV) {
            strncpy(env_store[env_count], argv[i], 255);
            env_count++;
        }
    }
    return 0;
}

static int builtin_alias(int argc, char *argv[]) {
    if (argc == 1) {
        for (int i = 0; i < g_nalias; i++)
            printf("alias %s='%s'\n", g_aliases[i].name, g_aliases[i].val);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { printf("alias: %s\n", argv[i]); continue; }
        *eq = '\0';
        if (g_nalias < MAX_ALIASES) {
            strncpy(g_aliases[g_nalias].name, argv[i], 63);
            strncpy(g_aliases[g_nalias].val, eq + 1, 255);
            char *v = g_aliases[g_nalias].val;
            int vl = (int)strlen(v);
            if (vl > 1 && (v[0] == '\'' || v[0] == '"') && v[vl-1] == v[0]) {
                memmove(v, v+1, vl-2);
                v[vl-2] = '\0';
            }
            g_nalias++;
        }
    }
    return 0;
}

static int builtin_unalias(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        for (int j = 0; j < g_nalias; j++) {
            if (strcmp(g_aliases[j].name, argv[i]) == 0) {
                for (int k = j; k < g_nalias - 1; k++) g_aliases[k] = g_aliases[k+1];
                g_nalias--;
                break;
            }
        }
    }
    return 0;
}

static int builtin_history(int argc, char *argv[]) {
    (void)argc; (void)argv;
    for (int i = 0; i < hist_len; i++)
        printf("%4d  %s\n", i + 1, history[i]);
    return 0;
}

static int builtin_exit(int argc, char *argv[]) {
    int code = argc > 1 ? atoi(argv[1]) : 0;
    _exit(code);
}

static int builtin_type(int argc, char *argv[]) {
    static const char *bnames[] = {
        "cd","exit","quit","export","alias","unalias",
        "history","type", NULL
    };
    for (int i = 1; i < argc; i++) {
        const char *al = alias_lookup(argv[i]);
        if (al) { printf("%s: alias for '%s'\n", argv[i], al); continue; }
        int is_bi = 0;
        for (int j = 0; bnames[j]; j++) {
            if (strcmp(bnames[j], argv[i]) == 0) { is_bi = 1; break; }
        }
        if (is_bi) { printf("%s: shell built-in\n", argv[i]); continue; }
        char path[256];
        if (find_in_path(argv[i], path, sizeof(path)) == 0)
            printf("%s is %s\n", argv[i], path);
        else
            printf("%s: not found\n", argv[i]);
    }
    return 0;
}

/* ============================================================
 * Execute a single command (no pipes)
 * Returns exit code of child
 * ============================================================ */

typedef struct {
    char *argv[MAX_ARGS];
    int   argc;
    char *redir_in;    /* < file */
    char *redir_out;   /* > file */
    int   append_out;  /* 1 if >> */
    int   background;  /* 1 if & */
} cmd_t;

/* Parse argv[] into cmd_t, extracting redirections */
static void parse_cmd(char **argv, int argc, cmd_t *cmd) {
    cmd->argc = 0;
    cmd->redir_in  = NULL;
    cmd->redir_out = NULL;
    cmd->append_out = 0;
    cmd->background = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "<") == 0 && i + 1 < argc) {
            cmd->redir_in = argv[++i];
        } else if (strcmp(argv[i], ">>") == 0 && i + 1 < argc) {
            cmd->redir_out = argv[++i]; cmd->append_out = 1;
        } else if (strcmp(argv[i], ">") == 0 && i + 1 < argc) {
            cmd->redir_out = argv[++i];
        } else if (strcmp(argv[i], "&") == 0) {
            cmd->background = 1;
        } else {
            cmd->argv[cmd->argc++] = argv[i];
        }
    }
    cmd->argv[cmd->argc] = NULL;
}

/* ---- Built-ins table ---- */
typedef int (*builtin_fn)(int, char **);
typedef struct { const char *name; builtin_fn fn; } builtin_t;

static const builtin_t builtins[] = {
    { "cd",       builtin_cd      },
    { "exit",     builtin_exit    },
    { "quit",     builtin_exit    },
    { "export",   builtin_export  },
    { "alias",    builtin_alias   },
    { "unalias",  builtin_unalias },
    { "history",  builtin_history },
    { "type",     builtin_type    },
    { NULL, NULL }
};

static int run_builtin(const char *name, int argc, char *argv[]) {
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(builtins[i].name, name) == 0)
            return builtins[i].fn(argc, argv);
    }
    return -1; /* not a builtin */
}

static int is_builtin_cmd(const char *name) {
    for (int i = 0; builtins[i].name; i++)
        if (strcmp(builtins[i].name, name) == 0) return 1;
    return 0;
}

static int execute_line(char *argv[], int argc);

static int run_script(const char *path, int argc, char *argv[]) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { printf("sh: %s: cannot open\n", path); return 126; }

    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { close(fd); return 0; }
    lseek(fd, 0, SEEK_SET);

    char *buf = (char *)malloc(sz + 1);
    if (!buf) { close(fd); printf("sh: out of memory\n"); return 126; }
    int n = read(fd, buf, sz);
    close(fd);
    if (n < 0) { free(buf); return 126; }
    buf[n] = '\0';

    /* Skip shebang line */
    char *p = buf;
    if (p[0] == '#' && p[1] == '!') {
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    /* Set script positional parameters: $0=script path, $1..$n=arguments */
    char *saved_argv0 = NULL;
    char old0buf[256];
    if (argc > 0) {
        saved_argv0 = argv[0];
        strncpy(old0buf, argv[0], sizeof(old0buf) - 1);
        old0buf[sizeof(old0buf) - 1] = '\0';
        argv[0] = (char *)path;
    }

    int ret = 0;
    while (*p) {
        char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = p - line_start;
        if (*p == '\n') p++;

        char line[MAX_LINE];
        if (line_len >= MAX_LINE) line_len = MAX_LINE - 1;
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        while (line_len > 0 && (line[line_len-1] == ' ' || line[line_len-1] == '\t'
              || line[line_len-1] == '\r')) { line[--line_len] = '\0'; }
        if (!line[0] || line[0] == '#') continue;

        char *toks[MAX_ARGS];
        int ntoks = tokenize(line, toks, MAX_ARGS);
        if (ntoks > 0) ret = execute_line(toks, ntoks);
    }

    if (saved_argv0) argv[0] = old0buf;
    free(buf);
    return ret;
}

static int execute_cmd(cmd_t *cmd) {
    if (cmd->argc == 0) return 0;

    const char *name = cmd->argv[0];

    /* Check alias */
    const char *al = alias_lookup(name);
    if (al) {
        char aline[MAX_LINE];
        strncpy(aline, al, sizeof(aline) - 1);
        aline[sizeof(aline) - 1] = '\0';
        int alen = (int)strlen(aline);
        for (int i = 1; i < cmd->argc; i++) {
            int avlen = (int)strlen(cmd->argv[i]);
            if (alen + 1 + avlen + 1 >= (int)sizeof(aline)) break;
            aline[alen++] = ' ';
            memcpy(aline + alen, cmd->argv[i], avlen);
            alen += avlen;
        }
        aline[alen] = '\0';
        char *aargv[MAX_ARGS];
        int aargc = tokenize(aline, aargv, MAX_ARGS);
        cmd_t acmd;
        parse_cmd(aargv, aargc, &acmd);
        return execute_cmd(&acmd);
    }

    /* ---- Built-in commands: redirect in parent process ---- */
    if (is_builtin_cmd(name)) {
        int saved_stdout = -1, saved_stdin = -1;

        if (cmd->redir_out) {
            int flags = O_WRONLY | O_CREAT | (cmd->append_out ? O_APPEND : O_TRUNC);
            int fd = open(cmd->redir_out, flags, 0644);
            if (fd < 0) { printf("sh: %s: cannot open\n", cmd->redir_out); return 1; }
            saved_stdout = dup(1);
            dup2(fd, 1);
            close(fd);
        }
        if (cmd->redir_in) {
            int fd = open(cmd->redir_in, O_RDONLY, 0);
            if (fd < 0) {
                printf("sh: %s: cannot open\n", cmd->redir_in);
                if (saved_stdout >= 0) { dup2(saved_stdout, 1); close(saved_stdout); }
                return 1;
            }
            saved_stdin = dup(0);
            dup2(fd, 0);
            close(fd);
        }

        int bret = run_builtin(name, cmd->argc, cmd->argv);

        if (saved_stdout >= 0) { dup2(saved_stdout, 1); close(saved_stdout); }
        if (saved_stdin >= 0) { dup2(saved_stdin, 0); close(saved_stdin); }
        return bret;
    }

    /* ---- External commands: redirect in child only ---- */

    char path[256];
    if (find_in_path(name, path, sizeof(path)) < 0) {
        printf("sh: %s: command not found\n", name);
        return 127;
    }

    /* Check if the file is a script (not ELF) */
    {
        int fd = open(path, O_RDONLY, 0);
        if (fd >= 0) {
            char magic[4];
            int n = read(fd, magic, 4);
            close(fd);
            if (n < 4 || magic[0] != 0x7f || magic[1] != 'E'
                       || magic[2] != 'L'  || magic[3] != 'F') {
                /* Not ELF — execute as shell script */
                return run_script(path, cmd->argc, cmd->argv);
            }
        }
    }

    /*
     * Save stdout/stdin BEFORE fork so we can restore them after waitpid.
     * The child's dup2() modifies the global g_files[] which is shared
     * with the parent — we must restore after the child finishes.
     */
    int saved_stdout = -1, saved_stdin = -1;
    if (cmd->redir_out) saved_stdout = dup(1);
    if (cmd->redir_in)  saved_stdin = dup(0);

    int pid = fork();
    if (pid < 0) { printf("sh: fork failed\n"); return 1; }

    if (pid == 0) {
        /* Child: set up redirections */
        if (cmd->redir_in) {
            int fd = open(cmd->redir_in, O_RDONLY, 0);
            if (fd < 0) { printf("sh: %s: cannot open\n", cmd->redir_in); _exit(1); }
            dup2(fd, 0); close(fd);
        }
        if (cmd->redir_out) {
            int flags = O_WRONLY | O_CREAT | (cmd->append_out ? O_APPEND : O_TRUNC);
            int fd = open(cmd->redir_out, flags, 0644);
            if (fd < 0) { printf("sh: %s: cannot open\n", cmd->redir_out); _exit(1); }
            dup2(fd, 1); close(fd);
        }

        /* Build envp */
        char *envp[MAX_ENV + 2];
        int ei = 0;
        for (char **e = environ; e && *e; e++) envp[ei++] = *e;
        for (int i = 0; i < env_count; i++) envp[ei++] = env_store[i];
        envp[ei] = NULL;

        execve(path, cmd->argv, envp);
        printf("sh: exec failed: %s\n", path);
        _exit(1);
    }

    if (cmd->background) {
        printf("[%d]\n", pid);
        return 0;
    }

    int status = 0;
    waitpid(pid, &status, 0);

    /* Restore after child's dup2 may have changed global g_files[] */
    if (saved_stdout >= 0) { dup2(saved_stdout, 1); close(saved_stdout); }
    if (saved_stdin >= 0)  { dup2(saved_stdin, 0);  close(saved_stdin); }

    return WEXITSTATUS(status);
}

/* ============================================================
 * Pipeline execution: cmd1 | cmd2 | ... | cmdN
 * ============================================================ */

static int execute_pipeline(char **argv, int argc) {
    /* Split by '|' */
    char *segments[16][MAX_ARGS];
    int   seg_lens[16];
    int   nseg = 0;
    int   start = 0;

    for (int i = 0; i <= argc; i++) {
        if (i == argc || strcmp(argv[i], "|") == 0) {
            if (i > start) {
                seg_lens[nseg] = i - start;
                for (int j = 0; j < seg_lens[nseg]; j++)
                    segments[nseg][j] = argv[start + j];
                segments[nseg][seg_lens[nseg]] = NULL;
                nseg++;
            }
            start = i + 1;
            if (nseg >= 16) break;
        }
    }

    if (nseg == 1) {
        cmd_t cmd;
        parse_cmd(segments[0], seg_lens[0], &cmd);
        return execute_cmd(&cmd);
    }

    /* Multi-stage pipeline */
    int pipefd[2];
    int prev_read = -1;
    int pids[16];

    for (int s = 0; s < nseg; s++) {
        int is_last = (s == nseg - 1);

        if (!is_last) {
            if (pipe(pipefd) < 0) { printf("sh: pipe failed\n"); return 1; }
        }

        int pid = fork();
        if (pid < 0) { printf("sh: fork failed\n"); return 1; }

        if (pid == 0) {
            /* Child */
            if (prev_read >= 0) { dup2(prev_read, 0); close(prev_read); }
            if (!is_last) { dup2(pipefd[1], 1); close(pipefd[1]); close(pipefd[0]); }

            cmd_t cmd;
            parse_cmd(segments[s], seg_lens[s], &cmd);

            /* Try built-in */
            int bret = run_builtin(cmd.argv[0], cmd.argc, cmd.argv);
            if (bret >= 0) _exit(bret);

            char path[256];
            if (find_in_path(cmd.argv[0], path, sizeof(path)) < 0) {
                printf("sh: %s: not found\n", cmd.argv[0]);
                _exit(127);
            }
            execve(path, cmd.argv, environ);
            _exit(1);
        }

        pids[s] = pid;
        if (prev_read >= 0) close(prev_read);
        if (!is_last) { close(pipefd[1]); prev_read = pipefd[0]; }
        else prev_read = -1;
    }

    int status = 0;
    for (int s = 0; s < nseg; s++) waitpid(pids[s], &status, 0);
    return WEXITSTATUS(status);
}

/* ============================================================
 * Execute a full command line (with &&, ||, ;)
 * ============================================================ */

static int execute_line(char *argv[], int argc) {
    if (argc == 0) return 0;

    /* Split on ; and handle && / || */
    int i = 0;
    int ret = 0;
    while (i < argc) {
        /* Find end of sub-command */
        int j = i;
        int op = 0; /* 0=none, 1=&&, 2=||, 3=; */
        while (j < argc) {
            if (strcmp(argv[j], "&&") == 0) { op = 1; break; }
            if (strcmp(argv[j], "||") == 0) { op = 2; break; }
            if (strcmp(argv[j], ";")  == 0) { op = 3; break; }
            j++;
        }

        /* Execute sub-command argv[i..j-1] */
        char *sub[MAX_ARGS];
        int sub_len = j - i;
        for (int k = 0; k < sub_len; k++) sub[k] = argv[i + k];
        sub[sub_len] = NULL;

        if (sub_len > 0) {
            /* Check for pipe */
            int has_pipe = 0;
            for (int k = 0; k < sub_len; k++)
                if (strcmp(sub[k], "|") == 0) { has_pipe = 1; break; }

            if (has_pipe) ret = execute_pipeline(sub, sub_len);
            else {
                cmd_t cmd;
                parse_cmd(sub, sub_len, &cmd);
                ret = execute_cmd(&cmd);
            }
            last_exit = ret;
        }

        if (op == 0) break;
        if (op == 1 && ret != 0) break; /* && short-circuit */
        if (op == 2 && ret == 0) break; /* || short-circuit */
        i = j + 1;
    }
    return ret;
}

/* ============================================================
 * Prompt
 * ============================================================ */

static void print_prompt(void) {
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    /* Shorten home */
    char *home = getenv("HOME");
    if (home) {
        size_t hl = strlen(home);
        if (strncmp(cwd, home, hl) == 0 && (cwd[hl] == '/' || cwd[hl] == '\0')) {
            char tmp[256] = "~";
            strcat(tmp, cwd + hl);
            strcpy(cwd, tmp);
        }
    }
    /* Colorized prompt: green user@host:path $ */
    printf("\033[1;32mA20OS\033[0m:\033[1;34m%s\033[0m\033[%sm$ \033[0m",
           cwd, last_exit ? "1;31" : "0");
}

/* ============================================================
 * main
 * ============================================================ */

int main(int argc, char *argv[], char *envp[]) {
    (void)argc; (void)argv;
    environ = envp;

    /* Set default environment */
    if (!getenv("PATH"))     { /* set via export */ }
    if (!getenv("HOME"))     { /* set via export */ }

    printf("A20 Shell  [Type 'help' for commands]\n\n");

    /* Install default aliases */
    snprintf(g_aliases[0].name, 64, "ll");
    snprintf(g_aliases[0].val,  256, "ls -la");
    snprintf(g_aliases[1].name, 64, "la");
    snprintf(g_aliases[1].val,  256, "ls -a");
    snprintf(g_aliases[2].name, 64, ".."); 
    snprintf(g_aliases[2].val,  256, "cd ..");
    g_nalias = 3;

    /* Add help as built-in */
    /* (handled below via the additional builtins check) */

    char line[MAX_LINE];

    while (1) {
        print_prompt();

        int len = readline_with_history(line, sizeof(line));
        if (len < 0) {
            printf("exit\n");
            break;
        }
        if (len == 0) continue;

        /* Trim trailing whitespace */
        while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t')) { line[--len] = '\0'; }
        if (!line[0]) continue;

        hist_add(line);

        /* Tokenize */
        char *toks[MAX_ARGS];
        int ntoks = tokenize(line, toks, MAX_ARGS);
        if (ntoks == 0) continue;

        last_exit = execute_line(toks, ntoks);
    }

    _exit(0);
}
