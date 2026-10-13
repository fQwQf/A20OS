#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int hold;
static unsigned ready;
static void *worker(void *unused) {
    (void)unused;
    unsigned long sum = 0;
    for (unsigned i = 1; i <= 1000; i++)
        sum += i;
    if (sum != 500500)
        _exit(93);
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (hold)
        pause();
    return NULL;
}
int main(int argc, char **argv) {
    const char *test = getenv("A20_TEST"), *empty = getenv("A20_EMPTY");
    if (argc != 5 || strcmp(argv[2], "hello world") || argv[3][0] ||
        strcmp(argv[4], "多架构") || !test || strcmp(test, "cluster") ||
        !empty || *empty)
        return 90;
    hold = !strcmp(argv[1], "hold");
    if (!hold && strcmp(argv[1], "exit"))
        return 91;
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++)
        if (pthread_create(&threads[i], NULL, worker, NULL))
            return 92;
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 2)
        usleep(1000);
    printf("APP-LINUX: PASS sum=500500 workers=2 argc=5 env=2 mode=%s\n",
           argv[1]);
    fflush(stdout);
    while (hold)
        pause();
    for (unsigned i = 0; i < 2; i++)
        if (pthread_join(threads[i], NULL))
            return 94;
    return 41;
}
