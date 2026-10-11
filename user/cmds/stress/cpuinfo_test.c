#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

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
    printf("CPUINFO: %s processors=%u\n", failed ? "FAIL" : "PASS", count);
    return failed;
}
