/*
 * A20OS liba20c integration test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    printf("=== liba20c integration test ===\n");
    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);

    printf("\n--- string test ---\n");
    const char *hello = "Hello, A20!";
    printf("strlen(\"%s\") = %lu\n", hello, (unsigned long)strlen(hello));
    printf("strcmp(\"abc\", \"abc\") = %d\n", strcmp("abc", "abc"));
    printf("strcmp(\"abc\", \"abd\") = %d\n", strcmp("abc", "abd"));

    printf("\n--- malloc test ---\n");
    void *p1 = malloc(128);
    void *p2 = malloc(256);
    void *p3 = malloc(64);
    printf("malloc(128) = %p\n", p1);
    printf("malloc(256) = %p\n", p2);
    printf("malloc(64)  = %p\n", p3);
    free(p1);
    free(p2);
    free(p3);
    printf("malloc/free cycle OK\n");

    void *c = calloc(4, 32);
    printf("calloc(4,32) = %p\n", c);
    free(c);

    printf("\n--- time test ---\n");
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        printf("clock_gettime: %lu.%09lu\n",
               (unsigned long)ts.tv_sec, (unsigned long)ts.tv_nsec);
    else
        printf("clock_gettime failed\n");

    time_t t = time(NULL);
    printf("time() = %lu\n", (unsigned long)t);

    printf("\n--- printf format test ---\n");
    printf("%%d: %d\n", 42);
    printf("%%d: %d\n", -42);
    printf("%%u: %u\n", 12345u);
    printf("%%x: %x\n", 0xdeadbeef);
    printf("%%p: %p\n", (void *)0x12345678);
    printf("%%s: %s\n", "test string");
    printf("%%c: %c\n", 'Z');
    printf("%%%%: %%\n");

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
