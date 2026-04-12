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

static int readline_with_history(char *buf, size_t sz) {
    int pos = 0;
    hist_idx = hist_len;

    while (1) {
        int c = getchar();
        if (c < 0) break;

        if (c == '\n' || c == '\r') {
            write(1, "\n", 1);
            buf[pos] = '\0';
            return pos;
        }

        if (c == 127 || c == '\b') { /* Backspace */
            if (pos > 0) {
                pos--;
                write(1, "\b \b", 3);
            }
            continue;
        }

        if (c == 4) { /* Ctrl+D */
            if (pos == 0) { write(1, "\n", 1); buf[0] = '\0'; return -1; }
            continue;
        }

        if (c == 3) { /* Ctrl+C */
            write(1, "^C\n", 3);
            buf[0] = '\0';
            return 0;
        }

        if (c == 12) { /* Ctrl+L: clear screen */
            write(1, "\033[2J\033[H", 7);
            /* Redraw prompt and current line */
            write(1, "\033[1;32m$ \033[0m", 13);
            write(1, buf, pos);
            continue;
        }

        if (c == 27) { /* Escape sequence (arrows) */
            int c2 = getchar();
            if (c2 == '[') {
                int c3 = getchar();
                if (c3 == 'A') { /* Up arrow */
                    if (hist_idx > 0) {
                        clear_line(pos);
                        hist_idx--;
                        strncpy(buf, history[hist_idx], sz - 1);
                        buf[sz - 1] = '\0';
                        pos = (int)strlen(buf);
                        write(1, buf, pos);
                    }
                } else if (c3 == 'B') { /* Down arrow */
                    clear_line(pos);
                    if (hist_idx < hist_len - 1) {
                        hist_idx++;
                        strncpy(buf, history[hist_idx], sz - 1);
                    } else {
                        hist_idx = hist_len;
                        buf[0] = '\0';
                    }
                    pos = (int)strlen(buf);
                    write(1, buf, pos);
                } else if (c3 == 'C') { /* Right */ }
                else if (c3 == 'D') { /* Left */
                    if (pos > 0) { pos--; write(1, "\b", 1); }
                }
            }
            continue;
        }

        if ((unsigned char)c < 32) continue; /* ignore other control chars */

        if ((size_t)pos < sz - 1) {
            buf[pos++] = (char)c;
            write(1, (char *)&c, 1); /* echo */
        }
    }
    buf[pos] = '\0';
    return pos;
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
    if (!path) path = "/bin:/usr/bin:/mnt/bin";
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
 * Built-in commands
 * ============================================================ */

static int builtin_cd(int argc, char *argv[]) {
    const char *dir = argc > 1 ? argv[1] : "/";
    if (chdir(dir) < 0) { printf("cd: %s: No such directory\n", dir); return 1; }
    return 0;
}

static int builtin_pwd(int argc, char *argv[]) {
    (void)argc; (void)argv;
    char buf[256];
    if (getcwd(buf, sizeof(buf))) printf("%s\n", buf);
    return 0;
}

static int builtin_echo(int argc, char *argv[]) {
    int newline = 1;
    int start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { newline = 0; start = 2; }
    for (int i = start; i < argc; i++) {
        if (i > start) write(1, " ", 1);
        /* Process escape sequences */
        const char *s = argv[i];
        while (*s) {
            if (*s == '\\' && s[1]) {
                s++;
                switch (*s) {
                    case 'n': putchar('\n'); break;
                    case 't': putchar('\t'); break;
                    case 'r': putchar('\r'); break;
                    case 'e': putchar(27); break;
                    case '\\': putchar('\\'); break;
                    default: putchar('\\'); putchar(*s); break;
                }
            } else {
                putchar(*s);
            }
            s++;
        }
    }
    if (newline) putchar('\n');
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
        /* Store in env_store */
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
            /* Remove surrounding quotes */
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
    int code = argc > 1 ? atoi(argv[1]) : last_exit;
    _exit(code);
}

static int builtin_type(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        const char *al = alias_lookup(argv[i]);
        if (al) { printf("%s: alias for '%s'\n", argv[i], al); continue; }
        char path[256];
        if (find_in_path(argv[i], path, sizeof(path)) == 0) printf("%s is %s\n", argv[i], path);
        else printf("%s: not found\n", argv[i]);
    }
    return 0;
}

static int builtin_env(int argc, char *argv[]) {
    (void)argc; (void)argv;
    for (char **e = environ; e && *e; e++) printf("%s\n", *e);
    for (int i = 0; i < env_count; i++) printf("%s\n", env_store[i]);
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
    { "pwd",      builtin_pwd     },
    { "echo",     builtin_echo    },
    { "exit",     builtin_exit    },
    { "quit",     builtin_exit    },
    { "export",   builtin_export  },
    { "alias",    builtin_alias   },
    { "unalias",  builtin_unalias },
    { "history",  builtin_history },
    { "type",     builtin_type    },
    { "env",      builtin_env     },
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

static int execute_cmd(cmd_t *cmd) {
    if (cmd->argc == 0) return 0;

    const char *name = cmd->argv[0];

    /* Check alias */
    const char *al = alias_lookup(name);
    if (al) {
        /* Re-tokenize aliased command */
        char aline[512];
        strncpy(aline, al, sizeof(aline) - 1);
        for (int i = 1; i < cmd->argc; i++) {
            strcat(aline, " ");
            strcat(aline, cmd->argv[i]);
        }
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

static int builtin_help(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("\033[1mBuilt-in commands:\033[0m\n");
    printf("  cd [dir]             Change directory\n");
    printf("  pwd                  Print working directory\n");
    printf("  echo [-n] [args]     Print text\n");
    printf("  export [VAR=val]     Set environment variable\n");
    printf("  alias [name=cmd]     Set/list aliases\n");
    printf("  unalias name         Remove alias\n");
    printf("  history              Show command history\n");
    printf("  type cmd             Show command location\n");
    printf("  env                  Print environment\n");
    printf("  exit [code]          Exit shell\n");
    printf("\n\033[1mExternal commands (in /bin):\033[0m\n");
    printf("  ls, cat, cp, rm, mkdir, ps, aed\n");
    printf("\n\033[1mSample files:\033[0m\n");
    printf("  /hello.txt           ramfs test file\n");
    printf("  /mnt/test.txt        FAT32 test file\n");
    printf("\n\033[1mFeatures:\033[0m\n");
    printf("  Pipelines:    cmd1 | cmd2\n");
    printf("  Redirection:  cmd > file, cmd >> file, cmd < file\n");
    printf("  Background:   cmd &\n");
    printf("  Conditionals: cmd1 && cmd2, cmd1 || cmd2\n");
    printf("  Variables:    $VAR, $?, $$\n");
    printf("  History:      Use arrow keys\n");
    return 0;
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

        /* Help is a special built-in */
        if (strcmp(toks[0], "help") == 0) { builtin_help(ntoks, toks); last_exit = 0; continue; }

        last_exit = execute_line(toks, ntoks);
    }

    _exit(last_exit);
}
