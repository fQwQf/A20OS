/*
 * ps — process table read back out of the kernel's per-pid procfs files.
 *
 * The kernel renders /proc/<pid>/stat in the Linux column layout (see
 * kernel/fs/procfs/procfs_render.c), so a row is recovered by position rather
 * than by name.  The line is "<pid> (<comm>) <state> <ppid> ...", and comm is
 * task->name verbatim: it may contain spaces and parentheses of its own, which
 * is why the numeric part is reached through the *last* ')' on the line rather
 * than the first.  Only three fields are actually wanted -- field 7 (tty_nr)
 * for TTY and fields 14/15 (utime/stime) for TIME -- but the walk has to step
 * over every field in between to reach them.
 *
 * utime/stime are scheduler accounting ticks, and proc_sched_tick() charges
 * them at a nominal 100 Hz -- the same 100 ticks = 1 s conversion the kernel's
 * own getrusage() applies -- so ticks/100 is the elapsed seconds and the
 * remainder is whole centiseconds.
 *
 * TTY is always "?".  task_t records no controlling terminal, so the kernel
 * hardcodes field 7 to 0 for every process; a zero tty_nr means "no terminal",
 * not terminal 0, so the honest rendering is the "?" Linux prints for an
 * untied process rather than an invented device number.
 *
 * CMD comes from /proc/<pid>/cmdline, which this kernel renders as the exec
 * path alone (there is no argv vector behind it to split on spaces) and leaves
 * empty for a task that never reached exec.  An empty cmdline falls back to the
 * bracketed [comm] form Linux uses for kernel threads, and a process that
 * disappears between the /proc scan and the read is simply dropped.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROC_ROOT "/proc"

/* /proc/<pid>/stat is a pid, a 64-byte comm in parentheses and two dozen
 * numbers, so 512 bytes is far more than the rendered line can need. */
#define STAT_MAX 512

/* /proc/<pid>/cmdline is task->exec_path (MAX_PATH_LEN, 512 bytes) followed by
 * a NUL, so the buffer needs one byte more than that to hold the terminator
 * fgets-style reads append. */
#define CMD_MAX 513

/* proc_sched_tick() accounting rate; the unit of utime_ticks/stime_ticks. */
#define TICKS_PER_SEC 100

/* Read a whole small procfs file.  Returns -1 when the file cannot be opened,
 * which for a per-pid file means the task exited mid-scan. */
static int read_proc_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f)
        return -1;
    n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

/* CPU time as [hh:]mm:ss, the shape ps uses.  Ticks are whole centiseconds
 * below a second, so there is nothing finer to show. */
static void format_time(char *out, size_t size, unsigned long ticks)
{
    unsigned long total = ticks / TICKS_PER_SEC;
    unsigned long sec = total % 60, min = (total / 60) % 60, hour = total / 3600;

    if (hour > 0)
        snprintf(out, size, "%lu:%02lu:%02lu", hour, min, sec);
    else
        snprintf(out, size, "%lu:%02lu", min, sec);
}

/* Linux packs tty_nr as major<<20|minor and ps shows only the minor.  A zero
 * tty_nr is "not attached to a terminal", which is every process on this
 * kernel, and prints as "?". */
static void format_tty(char *out, size_t size, unsigned long tty_nr)
{
    if (tty_nr == 0)
        snprintf(out, size, "?");
    else
        snprintf(out, size, "%lu", tty_nr & 0xfffffUL);
}

/* The command line, or the bracketed task name when there is none. */
static void format_cmd(int pid, char *out, size_t size)
{
    char path[64], buf[CMD_MAX], *nl;

    snprintf(path, sizeof(path), "%s/%d/cmdline", PROC_ROOT, pid);
    if (read_proc_file(path, buf, sizeof(buf)) > 0 && buf[0] != '\0') {
        /* A normal file would end in a newline; drop it if one showed up so
         * the column never spills onto the next row. */
        if ((nl = strchr(buf, '\n')) != NULL)
            *nl = '\0';
        snprintf(out, size, "%s", buf);
        return;
    }
    /* No arguments recorded: the task never reached exec, or its cmdline is
     * unreadable to us.  comm is the same name the stat line carries. */
    snprintf(path, sizeof(path), "%s/%d/comm", PROC_ROOT, pid);
    if (read_proc_file(path, buf, sizeof(buf)) < 0)
        buf[0] = '\0';
    if ((nl = strchr(buf, '\n')) != NULL)
        *nl = '\0';
    snprintf(out, size, "[%s]", buf[0] != '\0' ? buf : "?");
}

/* One table row, or nothing at all if the task went away while we looked. */
static void show_process(int pid)
{
    char path[64], line[STAT_MAX], tty[24], time[32], cmd[CMD_MAX];
    unsigned long tty_nr = 0, utime = 0, stime = 0;
    char *comm_end;
    const char *q;

    snprintf(path, sizeof(path), "%s/%d/stat", PROC_ROOT, pid);
    if (read_proc_file(path, line, sizeof(line)) < 0)
        return;
    /* Everything up to the last ')' is "<pid> (<comm>)".  comm is free-form
     * and may itself end in ')', but the field's own closing paren is always
     * the last one on the line, so scanning back for it is unambiguous. */
    comm_end = strrchr(line, ')');
    if (!comm_end)
        return;
    q = comm_end + 1;
    /* Field 3 is the single-letter state (R/S/T/Z/X), not a number, so it is
     * stepped over by hand rather than parsed. */
    while (*q == ' ')
        q++;
    if (*q == '\0' || *q == '\n')
        return;
    q++;
    for (int field = 4; field <= 15; field++) {
        char *end;
        unsigned long value;

        while (*q == ' ')
            q++;
        value = strtoul(q, &end, 10);
        if (end == q)
            return;  /* row is short: the task died while being rendered */
        if (field == 7)
            tty_nr = value;
        else if (field == 14)
            utime = value;
        else if (field == 15)
            stime = value;
        q = end;
    }

    format_tty(tty, sizeof(tty), tty_nr);
    format_time(time, sizeof(time), utime + stime);
    format_cmd(pid, cmd, sizeof(cmd));
    printf("%5d %-12s %-4s %s\n", pid, tty, time, cmd);
}

/* Only an all-digit name is a process directory.  Everything else the root
 * yields -- ".", "..", "self", "net", "sys", the various device dumps -- is
 * skipped, so "self" never turns into a bogus row; the caller's own process
 * shows up as its numeric directory like any other. */
static int is_pid_dir(const char *name)
{
    const char *p;

    if (name[0] < '0' || name[0] > '9')
        return 0;
    for (p = name; *p; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

static int compare_pids(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;

    return (x > y) - (x < y);
}

int main(int argc, char *argv[])
{
    struct dirent *entry;
    DIR *proc;
    int *pids, count = 0, size = 64, i;

    /* The selection flags are accepted and ignored: /proc/<pid> carries no
     * controlling terminal and no user ownership this ps could filter on, so
     * -e, -a and -x select the same rows here.  They are still parsed so that
     * "ps aux" and friends behave, while an option ps genuinely cannot honour
     * is reported instead of being silently dropped. */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            continue;  /* operand: there is no pid list to select from */
        if (argv[i][1] == '-' && argv[i][2] == '\0')
            break;     /* "--" ends the options */
        for (int j = 1; argv[i][j]; j++) {
            switch (argv[i][j]) {
            case 'e': case 'A': case 'a':
            case 'u': case 'x': case 'l':
                break;
            default:
                fprintf(stderr, "ps: unsupported option -%c\n", argv[i][j]);
                fprintf(stderr, "usage: ps [-e] [-a] [-A] [-l] [-u] [-x]\n");
                return 1;
            }
        }
    }

    proc = opendir(PROC_ROOT);
    if (!proc) {
        fprintf(stderr, "ps: cannot open %s\n", PROC_ROOT);
        return 1;
    }
    pids = malloc((size_t)size * sizeof(*pids));
    if (!pids) {
        closedir(proc);
        fprintf(stderr, "ps: out of memory\n");
        return 1;
    }
    while ((entry = readdir(proc)) != NULL) {
        if (!is_pid_dir(entry->d_name))
            continue;
        if (count == size) {
            int *grown = realloc(pids, (size_t)size * 2 * sizeof(*pids));

            if (!grown)
                break;  /* list what was collected before memory ran out */
            pids = grown;
            size *= 2;
        }
        pids[count++] = atoi(entry->d_name);
    }
    closedir(proc);

    /* The kernel walks the task list afresh per getdents() call, so a task
     * that moves within the list mid-scan can be handed out twice.  Sorting
     * makes any such repeat adjacent, which the loop below drops. */
    qsort(pids, (size_t)count, sizeof(*pids), compare_pids);
    printf("  PID TTY          TIME CMD\n");
    for (i = 0; i < count; i++) {
        if (i > 0 && pids[i] == pids[i - 1])
            continue;
        show_process(pids[i]);
    }
    free(pids);
    return 0;
}
