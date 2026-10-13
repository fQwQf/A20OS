/* Actual upstream-VM workload. JNI only observes the Native CPU; placement is
 * left to the ordinary Thread API and scheduler. Java 5 source/bytecode. */
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;

public final class A20NativeWorkload {
    private static native int cpu();
    static { System.loadLibrary("a20execution"); }
    private static final Object lock = new Object();
    private static int ready, next, completed, mask, total;
    private static boolean go;
    private static Throwable failure;
    private static final int[] results = new int[32];
    private static final int[] jobs = new int[2];

    private static void worker(int part) {
        try {
            synchronized (lock) {
                ready++;
                lock.notifyAll();
                while (!go) lock.wait();
            }
            for (;;) {
                int job;
                synchronized (lock) {
                    job = next++;
                }
                if (job >= 32) break;
                int hash = 0x811c9dc5;
                for (int i = 0; i < 8192; i++)
                    hash = (hash ^ (i & 255)) * 16777619;
                if (hash != 0xfd53fdc5)
                    throw new AssertionError("hash=" + Integer.toHexString(hash));
                int sum = 0;
                for (int i = 1; i <= 128; i++) sum += i * (job + 1);
                int current = cpu();
                if (current < 0 || current >= 32) throw new AssertionError("CPU range");
                synchronized (lock) {
                    if (results[job] != 0) throw new AssertionError("duplicate job");
                    results[job] = sum;
                    jobs[part]++;
                    completed++;
                    total += sum;
                    mask |= 1 << current;
                }
                Thread.yield();
            }
        } catch (Throwable error) {
            synchronized (lock) { failure = error; lock.notifyAll(); }
        }
    }

    public static void main(String[] args) throws Exception {
        Thread[] threads = new Thread[2];
        for (int i = 0; i < threads.length; i++) {
            final int part = i;
            threads[i] = new Thread(new Runnable() {
                public void run() { worker(part); }
            });
            threads[i].start();
        }
        synchronized (lock) {
            while (ready != 2 && failure == null) lock.wait();
            go = true;
            lock.notifyAll();
        }
        for (int i = 0; i < threads.length; i++) threads[i].join();
        if (failure != null) throw new AssertionError(failure);
        if (completed != 32 || total != 4359168 || jobs[0] == 0 || jobs[1] == 0)
            throw new AssertionError("incomplete work");
        for (int i = 0; i < results.length; i++)
            if (results[i] != 8256 * (i + 1)) throw new AssertionError("result");
        File file = new File("/tmp/a20-java-workload");
        FileOutputStream output = new FileOutputStream(file);
        try { output.write(new byte[] { 0x41, 0x32, 0x30 }); }
        finally { output.close(); }
        FileInputStream input = new FileInputStream(file);
        try {
            if (input.read() != 0x41 || input.read() != 0x32 || input.read() != 0x30 || input.read() != -1)
                throw new AssertionError("file data");
        } finally { input.close(); }
        if (!file.delete()) throw new AssertionError("file lifecycle");
        boolean missing = false;
        try { System.loadLibrary("a20_missing_jni_library"); }
        catch (UnsatisfiedLinkError expected) { missing = true; }
        if (!missing) throw new AssertionError("missing JNI accepted");
        System.out.println("JAVA_NATIVE: PASS jobs=" + completed + " sum=" + total
            + " mask=" + Integer.toHexString(mask) + " workers=" + jobs[0] + "/" + jobs[1]);
    }
}
