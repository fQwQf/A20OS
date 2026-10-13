/* Verification JNI: observe the real Native CPU without setting affinity. */
#include <jni.h>
#include "a20_task.h"

JNIEXPORT jint JNICALL Java_A20NativeWorkload_cpu(JNIEnv *env, jclass type) {
    (void)type;
    uint32_t cpu;
    if (a20_thread_get_cpu(&cpu) < 0 || cpu >= 32) {
        jclass error =
            (*env)->FindClass(env, "java/lang/IllegalStateException");
        if (error)
            (*env)->ThrowNew(env, error, "Native CPU query failed");
        return -1;
    }
    return (jint)cpu;
}
