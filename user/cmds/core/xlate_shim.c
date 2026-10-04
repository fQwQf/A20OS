/*
 * A20OS — a stand-in translator, for testing the forwarding channel itself
 *
 * This is NOT a translator.  It translates nothing; it prints the argv and
 * the environment it was handed and exits 0.  Its only job is to make the
 * *invocation* observable, so a test can assert exactly what the kernel built
 * when it forwarded a foreign image.
 *
 * Why it exists
 * -------------
 * The channel's real translator is qemu-user, which is a 3.5 MB download, is
 * only built when XLATOR=1, and *works*, so a test asserting on its argv
 * proves much less than it looks like it proves: it cannot distinguish "the
 * kernel built the argv the template asked for" from "qemu-user happened to
 * tolerate this argv".  A stand-in that does nothing but print has no such
 * tolerance -- if the kernel gets the argv wrong, the output is visibly
 * wrong.
 *
 * It is also what makes a *differently shaped* translator testable at all.
 * The whole point of the argv template is that a wrapper which wants
 *
 *     rttranslator <path> <args...>
 *
 * (the image as argv[0], no option) is a configuration away from
 *
 *     qemu-x86_64 -0 <argv0> <path> <args...>
 *
 * and there is no qemu to point at for the first shape.  This program is that
 * target.  With `a20.xlator.x86_64.argv=@P,@*` the kernel must hand it
 *
 *     argv[0] = /bin/xlate_shim
 *     argv[1] = <guest path>
 *     argv[2..] = <guest args>
 *
 * with no stray argument in front of the path -- which is precisely what the
 * old per-guest "argv0 flag" column could not express, since the best it
 * could do was suppress the flag and still leave a positional there.
 *
 * Output is line-oriented and greppable, and deliberately includes a
 * sentinel so a log where the shim never ran cannot be mistaken for a log
 * where it ran and printed nothing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exits with this so a caller can tell "the shim ran to completion" from
 * "something else exited 0". */
#define SHIM_EXIT 0

/* The environment variables worth reporting.  A translator cannot usefully be
 * asserted on by dumping all of environ -- that would make the expected
 * output depend on whatever the init process happened to export -- so the
 * kernel-injected ones are named explicitly.  This is the variable
 * smoke-exec-xlator-shim configures through a20.xlator.<guest>.env. */
static const char *const REPORTED_ENV[] = {
    "XLATOR_TEST_ENV",
    "ROSETTA_TMPDIR",
    NULL,
};

int main(int argc, char **argv)
{
    printf("XLATE_SHIM: started argc=%d\n", argc);

    /* argv[0] first and separately: it is the one the kernel chose on its own
     * (the translator path), so it is the one a broken rewrite shows up in. */
    printf("XLATE_SHIM: argv[0]=%s\n", argc > 0 ? argv[0] : "(none)");

    for (int i = 1; i < argc; i++)
        printf("XLATE_SHIM: argv[%d]=%s\n", i, argv[i]);

    for (int i = 0; REPORTED_ENV[i]; i++) {
        const char *v = getenv(REPORTED_ENV[i]);
        printf("XLATE_SHIM: env %s=%s\n", REPORTED_ENV[i], v ? v : "(unset)");
    }

    printf("XLATE_SHIM: done\n");
    return SHIM_EXIT;
}
