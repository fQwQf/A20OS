#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    cpu_set_t affinity, listed;
    CPU_ZERO(&affinity);
    CPU_ZERO(&listed);
    if (sched_getaffinity(0, sizeof(affinity), &affinity))
        return puts("CPUINFO: FAIL affinity"), 1;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return puts("CPUINFO: FAIL open"), 1;
    char line[512];
    unsigned count = 0, cpu;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "processor : %u", &cpu) != 1)
            continue;
        if (cpu >= CPU_SETSIZE || CPU_ISSET(cpu, &listed))
            return puts("CPUINFO: FAIL duplicate or invalid CPU"), 1;
        CPU_SET(cpu, &listed);
        count++;
    }
    int failed = ferror(f);
    fclose(f);
    for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &affinity) && !CPU_ISSET(cpu, &listed))
            failed = 1;
    if (!count || (argc > 1 && count != strtoul(argv[1], NULL, 10)))
        failed = 1;
    if (!failed && argc > 2) {
        for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (!CPU_ISSET(cpu, &affinity))
                continue;
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(cpu, &one);
            int ready[2], go[2];
            if (pipe(ready) || pipe(go)) {
                failed = 1;
                break;
            }
            pid_t child = fork();
            if (child == 0) {
                close(ready[0]);
                close(go[1]);
                char byte = 1;
                if (write(ready[1], &byte, 1) != 1 ||
                    read(go[0], &byte, 1) != 1)
                    _exit(2);
                for (unsigned i = 0; i < 200; i++)
                    if (sched_getcpu() != (int)cpu)
                        _exit(3);
                _exit(0);
            }
            close(ready[1]);
            close(go[0]);
            char byte;
            if (child < 0 || read(ready[0], &byte, 1) != 1) {
                failed = 1;
                break;
            }
            /* Set affinity while the child sleeps, before it is dispatched. */
            usleep(10000);
            if (sched_setaffinity(child, sizeof(one), &one))
                failed = 1;
            if (write(go[1], &byte, 1) != 1)
                failed = 1;
            int status;
            if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
                WEXITSTATUS(status))
                failed = 1;
            close(ready[0]);
            close(go[1]);
        }
        struct timespec begin, end;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        for (unsigned i = 0; i < 20000; i++)
            (void)getpid();
        clock_gettime(CLOCK_MONOTONIC, &end);
        int64_t ns = (end.tv_sec - begin.tv_sec) * 1000000000LL + end.tv_nsec -
                     begin.tv_nsec;
        printf("CPUINFO_BENCH: getpid 20000 calls %lld us\n",
               (long long)(ns / 1000));
    }
    printf("CPUINFO: %s processors=%u\n", failed ? "FAIL" : "PASS", count);
    return failed;
}
