/* A real VM GC workload. Workers retain private graphs on Java stacks and run
 * pure Java loops without JNI, allocation, yielding, sleeping or monitors while
 * the main thread forces collections and allocation-driven heap pressure. */
import java.lang.ref.WeakReference;

public final class A20NativeGC {
    private static final Object lock = new Object();
    private static int ready;
    private static boolean go;
    private static volatile boolean stop;
    private static volatile long progress0, progress1;
    private static volatile Throwable failure;
    private static volatile boolean sleepReady, interrupted;
    private static volatile byte[] sink;
    private static final boolean[] verified = new boolean[2];

    private static final class Node {
        int id;
        int[] words = new int[32];
        Node next;
    }

    private static void worker(int part) {
        try {
            Node[] nodes = new Node[64];
            for (int i = 0; i < nodes.length; i++) {
                Node node = nodes[i] = new Node();
                node.id = part * 1000 + i;
                for (int j = 0; j < node.words.length; j++)
                    node.words[j] = (node.id * 31) ^ j;
            }
            for (int i = 0; i < nodes.length; i++)
                nodes[i].next = nodes[(i + 1) % nodes.length];
            Node root = nodes[0];
            synchronized (lock) {
                ready++;
                lock.notifyAll();
                while (!go) lock.wait();
            }
            while (!stop) {
                Node node = root;
                for (int i = 0; i < 64; i++, node = node.next) {
                    if (node.id != part * 1000 + i)
                        throw new AssertionError("live graph identity");
                    for (int j = 0; j < node.words.length; j++)
                        if (node.words[j] != ((node.id * 31) ^ j))
                            throw new AssertionError("live graph data");
                }
                if (node != root) throw new AssertionError("live graph cycle");
                if (part == 0) progress0++; else progress1++;
            }
            if (root != nodes[0]) throw new AssertionError("stack root");
            verified[part] = true;
        } catch (Throwable error) {
            failure = error;
            stop = true;
            synchronized (lock) { lock.notifyAll(); }
        }
    }

    private static WeakReference unreachable() {
        return new WeakReference(new Object());
    }

    private static void advanced(long before0, long before1) {
        while ((progress0 <= before0 || progress1 <= before1) && failure == null)
            Thread.yield();
        if (failure != null) throw new AssertionError(failure);
    }

    public static void main(String[] args) throws Exception {
        Thread[] threads = new Thread[2];
        for (int i = 0; i < 2; i++) {
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
        advanced(0, 0);
        System.out.println("JAVA_GC: collecting with two pure Java workers");
        WeakReference[] dead = new WeakReference[16];
        for (int round = 0; round < 16; round++) {
            dead[round] = unreachable();
            long before0 = progress0, before1 = progress1;
            System.gc();
            advanced(before0, before1);
            for (int i = 0; i < 128; i++) {
                byte[] bytes = new byte[32768];
                bytes[0] = (byte)round;
                bytes[bytes.length - 1] = (byte)~round;
                sink = bytes;
            }
            System.gc();
            if (sink[0] != (byte)round || sink[sink.length - 1] != (byte)~round)
                throw new AssertionError("live array data");
            advanced(before0, before1);
        }
        int reclaimed = 0;
        for (int i = 0; i < dead.length; i++)
            if (dead[i].get() == null) reclaimed++;
        stop = true;
        for (int i = 0; i < 2; i++) threads[i].join();
        if (failure != null) throw new AssertionError(failure);
        if (!verified[0] || !verified[1] || reclaimed < 1)
            throw new AssertionError("GC roots/reclamation");
        Thread sleeper = new Thread(new Runnable() {
            public void run() {
                try {
                    sleepReady = true;
                    Thread.sleep(60000);
                    failure = new AssertionError("sleep was not interrupted");
                } catch (InterruptedException expected) {
                    interrupted = !Thread.currentThread().isInterrupted();
                }
            }
        });
        sleeper.start();
        while (!sleepReady) Thread.yield();
        sleeper.interrupt();
        sleeper.join();
        if (failure != null) throw new AssertionError(failure);
        if (!interrupted) throw new AssertionError("ordinary Thread interrupt status");
        System.out.println("JAVA_GC: PASS rounds=16 allocated=67108864 roots=128 reclaimed="
            + reclaimed + " ticks=" + progress0 + "/" + progress1);
    }
}
