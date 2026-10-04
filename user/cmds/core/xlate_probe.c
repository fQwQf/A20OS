/*
 * A20OS — foreign-architecture translation probe
 *
 * The payload half of the execve translation smoke.  It is deliberately
 * trivial: a fixed marker on stdout and a characteristic exit code, so
 * the smoke can assert both that the guest ran and that its exit status
 * reached the parent unmangled.
 *
 * Built twice from this one source:
 *   - natively, by user/Makefile, as `xlate_probe` — the control group;
 *   - for x86_64, by `make xlate-probe-x86_64`, as
 *     `xlate_probe-x86_64` — the translated guest.
 *
 * Both copies print the same marker and return the same code, so a
 * difference between them can only come from the translation path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Characteristic exit code; must be distinguishable from a translator
 * failure (127) and from a signal death. */
#define XLATE_PROBE_EXIT 42

int main(int argc, char **argv)
{
    const char *marker = (argc > 1) ? argv[1] : "no-arg";

    printf("XLATE_PROBE: MARK=%s ARGC=%d\n", marker, argc);
    for (int i = 1; i < argc; i++)
        printf("XLATE_PROBE: ARGV[%d]=%s\n", i, argv[i]);
    fflush(stdout);

    /* Optional "S<seconds>" argument: keeps the process alive after it has
     * produced its output, which separates "did the work" from "was the
     * parent still able to collect the result". */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == 'S' && argv[i][1] != '\0') {
            long secs = strtol(argv[i] + 1, NULL, 10);
            sleep((unsigned)secs);
        }
    }

    return XLATE_PROBE_EXIT;
}