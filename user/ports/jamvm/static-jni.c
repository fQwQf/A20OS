/* Statically linked JNI modules; same libc, TLS and Native thread scheduler.
 * JamVM retains normal class-loader ownership and JNI method invocation.
 * Modules are pinned for the executable lifetime; there is no DSO mapping.
 */
#include <string.h>
#include "static-jni.h"

void *a20_jni_open(const char *path) {
    if (!path)
        return NULL;
    for (size_t i = 0; i < a20_jni_module_count; i++)
        if (!strcmp(path, a20_jni_modules[i].path))
            return (void *)&a20_jni_modules[i];
    return NULL;
}

int a20_jni_is_static(void *handle) {
    for (size_t i = 0; i < a20_jni_module_count; i++)
        if (handle == &a20_jni_modules[i])
            return 1;
    return 0;
}

void *a20_jni_symbol(void *handle, const char *name) {
    if (!a20_jni_is_static(handle) || !name)
        return NULL;
    const A20JNIModule *module = handle;
    for (size_t i = 0; i < module->count; i++)
        if (!strcmp(name, module->symbols[i].name))
            return (void *)module->symbols[i].address;
    return NULL;
}
