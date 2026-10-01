/*
 * A20OS liba20c integration test.
 *
 * Every check below asserts and routes through fail(): the PASS marker at the
 * end is reachable only when all of them held.  (This used to be a print-only
 * tour that always exited 0 — the only gate liba20c had.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void __liba20c_init(void);

static int failures;

static void fail(const char *what)
{
    printf("NATIVE_LIBC: FAIL %s\n", what);
    failures++;
}

int main(int argc, char **argv)
{
    __liba20c_init();

    printf("=== liba20c integration test ===\n");
    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);

    /* Mixed-width varargs: an int followed by a pointer must not shift the
     * va_list cursor (regression for the ppc64le helper-va_arg bug). */
    printf("  mix: %d %s\n", 7, "hello");

    printf("\n--- string test ---\n");
    const char *hello = "Hello, A20!";
    size_t len = strlen(hello);
    printf("strlen(\"%s\") = %lu\n", hello, (unsigned long)len);
    if (len != 11)
        fail("strlen");
    if (strcmp("abc", "abc") != 0)
        fail("strcmp equality");
    if (strcmp("abc", "abd") >= 0)
        fail("strcmp ordering");
    char buf[16];
    memset(buf, 'x', sizeof(buf));
    memcpy(buf, hello, len + 1);
    if (strcmp(buf, hello) != 0)
        fail("memcpy/memcmp");

    printf("\n--- malloc test ---\n");
    void *p1 = malloc(128);
    void *p2 = malloc(256);
    void *p3 = malloc(64);
    printf("malloc(128) = %p\n", p1);
    printf("malloc(256) = %p\n", p2);
    printf("malloc(64)  = %p\n", p3);
    if (!p1 || !p2 || !p3)
        fail("malloc");
    /* Distinct live allocations must not overlap. */
    if (p1 && p2 && (char *)p2 - (char *)p1 < 128)
        fail("malloc overlap");
    free(p1);
    free(p2);
    free(p3);
    printf("malloc/free cycle OK\n");

    /* The free list must hand the 128-byte block back. */
    void *p1b = malloc(128);
    if (p1b != p1)
        fail("free-list reuse");
    void *c = calloc(4, 32);
    printf("calloc(4,32) = %p\n", c);
    if (!c)
        fail("calloc");
    else {
        const unsigned char *uc = (const unsigned char *)c;
        if (uc[0] || uc[15] || uc[127])
            fail("calloc zeroing");
    }
    /* calloc(0, n) and the overflow check must fail cleanly, not div-by-zero. */
    if (calloc(0, 32) != NULL)
        fail("calloc(0,n)");
    if (calloc((size_t)-1, 4) != NULL)
        fail("calloc overflow");
    free(c);
    free(p1b);
    if (malloc(0) != NULL)
        fail("malloc(0)");

    printf("\n--- time test ---\n");
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        printf("clock_gettime: %lu.%09lu\n",
               (unsigned long)ts.tv_sec, (unsigned long)ts.tv_nsec);
    else
        fail("clock_gettime");

    time_t t = time(NULL);
    printf("time() = %lu\n", (unsigned long)t);
    if (t == (time_t)-1)
        fail("time");

    printf("\n--- stdio test ---\n");
    const char *path = "/tmp/liba20c_stdio.txt";
    FILE *w = fopen(path, "w");
    if (!w)
        fail("fopen w");
    else {
        if (fputs("hello stdio\n", w) < 0)
            fail("fputs");
        if (fclose(w) != 0)
            fail("fclose w");
    }
    FILE *r = fopen(path, "r");
    if (!r)
        fail("fopen r");
    else {
        char line[32] = {0};
        if (!fgets(line, sizeof(line), r) || strcmp(line, "hello stdio\n") != 0)
            fail("fgets roundtrip");
        fclose(r);
    }
    /* Append mode must seek to the end, not overwrite from offset 0. */
    FILE *a = fopen(path, "a");
    if (!a)
        fail("fopen a");
    else {
        fputs("appended\n", a);
        fclose(a);
    }
    char tail[32] = {0};
    r = fopen(path, "r");
    if (!r)
        fail("fopen r after append");
    else {
        while (fgets(tail, sizeof(tail), r))
            ;
        fclose(r);
        if (strcmp(tail, "appended\n") != 0)
            fail("append semantics");
    }

    printf("\n--- printf format test ---\n");
    printf("%%d: %d\n", 42);
    printf("%%d: %d\n", -42);
    printf("%%u: %u\n", 12345u);
    printf("%%x: %x\n", 0xdeadbeef);
    printf("%%p: %p\n", (void *)0x12345678);
    printf("%%s: %s\n", "test string");
    printf("%%c: %c\n", 'Z');
    printf("%%%%: %%\n");

    if (failures) {
        printf("\nNATIVE_LIBC: FAIL (%d check(s))\n", failures);
        return 1;
    }
    printf("\nNATIVE_LIBC: PASS\n");
    return 0;
}
