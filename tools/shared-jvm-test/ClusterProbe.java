import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicIntegerArray;
import java.util.concurrent.atomic.AtomicReference;

public class ClusterProbe {
    // JNI only observes the ordinary scheduler; no A20 cluster-specific API.
    static { System.load("/a20-cluster-payload/libclusterprobe.so"); }
    private static native int cpu();
    private static native int bind(int cpu);
    private static final int JOBS = 512;
    private static void check(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }
    public static void main(String[] args) throws Exception {
        AtomicInteger cursor = new AtomicInteger();
        AtomicInteger mask = new AtomicInteger();
        AtomicInteger moves = new AtomicInteger();
        AtomicIntegerArray visits = new AtomicIntegerArray(JOBS);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch ready = new CountDownLatch(2), start = new CountDownLatch(1);
        long[] sums = new long[2];
        int[] counts = new int[2];
        Thread[] workers = new Thread[2];
        for (int i = 0; i < 2; i++) {
            final int id = i;
            workers[i] = new Thread(() -> {
                boolean announced = false;
                try {
                    for (int round = 0; round < 8; round++) {
                        int target = (id + round) & 1;
                        check(bind(target) == 0 && cpu() == target, "Java thread affinity");
                        moves.incrementAndGet();
                    }
                    check(bind(id) == 0, "worker bind");
                    // Each worker reserves its first real job before either
                    // starts consuming the shared queue. Remaining jobs are
                    // dynamically claimed, not preassigned partitions.
                    int job = cursor.getAndIncrement();
                    ready.countDown(); announced = true;
                    start.await();
                    do {
                        check(cpu() == id, "Java worker moved outside its CPU mask");
                        mask.getAndUpdate(value -> value | (1 << cpu()));
                        check(visits.getAndIncrement(job) == 0, "duplicate job");
                        byte[] allocation = new byte[128];
                        long sum = 0;
                        for (int k = 1; k <= 128; k++) {
                            allocation[k - 1] = (byte)k;
                            sum += (long)(allocation[k - 1] & 255) * (job + 1);
                        }
                        sums[id] += sum; counts[id]++;
                        Thread.yield();
                        job = cursor.getAndIncrement();
                    } while (job < JOBS);
                } catch (Throwable error) {
                    failure.compareAndSet(null, error);
                } finally {
                    if (!announced) ready.countDown();
                }
            }, "a20-java-worker-" + id);
            workers[i].start();
        }
        ready.await(); start.countDown();
        for (Thread worker : workers) worker.join();
        if (failure.get() != null) throw new AssertionError("worker failed", failure.get());
        for (int job = 0; job < JOBS; job++) check(visits.get(job) == 1, "missing job");
        long expected = 8256L * JOBS * (JOBS + 1) / 2;
        check(sums[0] + sums[1] == expected && counts[0] > 0 && counts[1] > 0 &&
              counts[0] + counts[1] == JOBS && mask.get() == 3 && moves.get() == 16,
              "shared Java work distribution");
        System.out.println("CLUSTER_JAVA: PASS threads=2 affinity-checks=16 mask=3 jobs=" + JOBS +
            " split=" + counts[0] + "/" + counts[1] + " sum=" + expected);
    }
}
