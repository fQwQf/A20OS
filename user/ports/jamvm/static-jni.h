/* Optional statically linked JNI modules for Native JamVM. */
#ifndef A20_JAMVM_STATIC_JNI_H
#define A20_JAMVM_STATIC_JNI_H
#include <stddef.h>

typedef struct {
    const char *name;
    void (*address)(void);
} A20JNISymbol;

typedef struct {
    const char *path;
    const A20JNISymbol *symbols;
    size_t count;
} A20JNIModule;

/* Generated from actual defined JNI symbols in the linked upstream objects. */
extern const A20JNIModule a20_jni_modules[];
extern const size_t a20_jni_module_count;
void *a20_jni_open(const char *path);
int a20_jni_is_static(void *handle);
void *a20_jni_symbol(void *handle, const char *name);
#endif
