/* Exercise the actual upstream JamVM/libffi JNI call adapter on Native A20OS.
 * This is a C ABI probe, not execution of Java bytecode or a JVM substitute.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jam.h"

extern uintptr_t *callJNIMethod(void *, Class *, char *, int, uintptr_t *,
                                unsigned char *);
static int env_token, class_token, object_token, calls, errors;

void signalChainedExceptionEnum(int id, char *message, Object *cause) {
    (void)id;
    (void)cause;
    fprintf(stderr, "JAMVM_JNI: FAIL adapter exception %s\n", message);
    exit(90);
}

static void context(void *env, void *object) {
    calls++;
    if (env != &env_token || object != &class_token)
        errors++;
}

static int64_t mixed(void *env, void *clazz, int32_t i, int8_t b, int16_t s,
                     uint16_t c, uint8_t z, int64_t j, float f, double d,
                     void *object) {
    context(env, clazz);
    if (i != -1234567 || b != -117 || s != -30001 || c != 0xf234 || z != 1 ||
        j != -INT64_C(0x123456789abcdef) || f != -13.25f || d != 42.125 ||
        object != &object_token)
        errors++;
    return -INT64_C(0x102030405060708);
}

#define RETURN_TEST(name, type, value)                                         \
    static type name(void *env, void *clazz) {                                 \
        context(env, clazz);                                                   \
        return (value);                                                        \
    }
RETURN_TEST(ret_byte, int8_t, -117)
RETURN_TEST(ret_short, int16_t, -30001)
RETURN_TEST(ret_char, uint16_t, 0xf234)
RETURN_TEST(ret_bool, uint8_t, 1)
RETURN_TEST(ret_int, int32_t, -1234567)
RETURN_TEST(ret_float, float, -13.25f)
RETURN_TEST(ret_double, double, 42.125)
RETURN_TEST(ret_object, void *, &object_token)
static void ret_void(void *env, void *clazz) {
    context(env, clazz);
}
static int32_t instance(void *env, void *object, int32_t i) {
    calls++;
    if (env != &env_token || object != &object_token || i != -44)
        errors++;
    return 77;
}

int main(void) {
    uintptr_t stack[16] = {0};
    float f = -13.25f;
    double d = 42.125;
    stack[0] = (uintptr_t)-1234567;
    stack[1] = (uintptr_t)-117;
    stack[2] = (uintptr_t)-30001;
    stack[3] = 0xf234;
    stack[4] = 1;
    stack[5] = (uintptr_t)-INT64_C(0x123456789abcdef);
    memcpy(&stack[7], &f, sizeof(f));
    memcpy(&stack[8], &d, sizeof(d));
    stack[10] = (uintptr_t)&object_token;
    uintptr_t *end = callJNIMethod(&env_token, (Class *)&class_token,
                                   "(IBSCZJFDLjava/lang/Object;)J", 11, stack,
                                   (unsigned char *)mixed);
    int64_t wide;
    memcpy(&wide, stack, sizeof(wide));
    if (end != stack + 2 || wide != -INT64_C(0x102030405060708))
        errors++;

#define CHECK_INT_RETURN(sig, fn, expected)                                    \
    do {                                                                       \
        memset(stack, 0xa5, sizeof(stack));                                    \
        end = callJNIMethod(&env_token, (Class *)&class_token, (sig), 2,       \
                            stack, (unsigned char *)(fn));                     \
        if (end != stack + 1 || stack[0] != (uintptr_t)(expected))             \
            errors++;                                                          \
    } while (0)
    CHECK_INT_RETURN("()B", ret_byte, -117);
    CHECK_INT_RETURN("()S", ret_short, -30001);
    CHECK_INT_RETURN("()C", ret_char, 0xf234);
    CHECK_INT_RETURN("()Z", ret_bool, 1);
    CHECK_INT_RETURN("()I", ret_int, -1234567);
    CHECK_INT_RETURN("()Ljava/lang/Object;", ret_object, &object_token);
    end = callJNIMethod(&env_token, (Class *)&class_token, "()F", 2, stack,
                        (unsigned char *)ret_float);
    memcpy(&f, stack, sizeof(f));
    if (end != stack + 1 || f != -13.25f)
        errors++;
    end = callJNIMethod(&env_token, (Class *)&class_token, "()D", 2, stack,
                        (unsigned char *)ret_double);
    memcpy(&d, stack, sizeof(d));
    if (end != stack + 2 || d != 42.125)
        errors++;
    end = callJNIMethod(&env_token, (Class *)&class_token, "()V", 2, stack,
                        (unsigned char *)ret_void);
    if (end != stack)
        errors++;
    stack[0] = (uintptr_t)&object_token;
    stack[1] = (uintptr_t)-44;
    end = callJNIMethod(&env_token, NULL, "(I)I", 3, stack,
                        (unsigned char *)instance);
    if (end != stack + 1 || stack[0] != 77)
        errors++;
    if (errors || calls != 11) {
        printf("JAMVM_JNI: FAIL calls=%d errors=%d\n", calls, errors);
        return 1;
    }
    puts("JAMVM_JNI: PASS actual adapter static instance integers float double "
         "object void calls=11");
    return 0;
}
