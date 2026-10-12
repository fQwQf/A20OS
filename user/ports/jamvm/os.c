/* Native OS adapter for JamVM 2.0.0, GPL-2.0-or-later.
 * Uses the same execution-domain API as Native MCU/server applications.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include "a20_execution.h"
#include "jam.h"

void initialisePlatform(void) {
}

long long nativePhysicalMemory(void) {
    long pages = sysconf(_SC_PHYS_PAGES), bytes = sysconf(_SC_PAGESIZE);
    return pages > 0 && bytes > 0 ? (long long)pages * bytes : 0;
}

/* The command-line launcher supplies its live outer frame to JamVM itself.
 * JNI invocation/attachment needs a real stack-bound query before admission;
 * a guessed address or NULL could let conservative GC miss roots. */
void *nativeStackBase(void) {
    fputs("JamVM Native: JNI attachment requires stack-bound support\n",
          stderr);
    exit(1);
}

int nativeAvailableProcessors(void) {
    a20_execution_domain_info_t domain = {0};
    domain.size = sizeof(domain);
    domain.version = A20_DOMAIN_VERSION;
    if (a20_execution_domain_info(&domain) < 0 ||
        !a20_execution_domain_valid(&domain))
        return 1;
    return domain.cpu_online;
}

char *nativeLibError(void) {
    return dlerror();
}
char *nativeLibPath(void) {
    return getenv("LD_LIBRARY_PATH");
}
void *nativeLibOpen(char *path) {
    return dlopen(path, RTLD_LAZY);
}
void nativeLibClose(void *handle) {
    dlclose(handle);
}
void *nativeLibSym(void *handle, char *name) {
    return dlsym(handle, name);
}
char *nativeLibMapName(char *name) {
    char *path = sysMalloc(strlen(name) + sizeof("lib.so"));
    sprintf(path, "lib%s.so", name);
    return path;
}
char *nativeJVMPath(void) {
    const char path[] = INSTALL_DIR "/bin/jamvm";
    char *copy = sysMalloc(sizeof(path));
    memcpy(copy, path, sizeof(path));
    return copy;
}
