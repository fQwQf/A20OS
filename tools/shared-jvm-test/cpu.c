/* Two primitive JNI methods: JNIEnv* and jclass are opaque pointer arguments;
 * neither method calls into the VM. Java int and the return type are 32 bits.
 * Leave ordinary libc scheduler functions unresolved for the guest's musl
 * loader; do not link the host's libc or introduce a cluster-specific syscall. */
extern int sched_getcpu(void);
extern int sched_setaffinity(int pid, unsigned long size, const void *mask);
_Static_assert(sizeof(int) == 4, "JNI int width");
int Java_ClusterProbe_cpu(void *env, void *clazz) {
    (void)env; (void)clazz;
    return sched_getcpu();
}
int Java_ClusterProbe_bind(void *env, void *clazz, int cpu) {
    (void)env; (void)clazz;
    if (cpu < 0 || cpu > 1) return -1;
    unsigned long mask = 1UL << cpu;
    return sched_setaffinity(0, sizeof(mask), &mask);
}
