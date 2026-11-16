/* Guest application contract for the packaged OpenJDK and LWJGL runtime.
 * Compile with Java 21, then run in the desktop guest:
 * java -cp <class-directory> A20JvmRuntime <minecraft-native-directory>
 * The PASS line reports the shutdown-hook path; check its contents after exit.
 */
import java.io.*;
import java.net.*;
import java.nio.*;
import java.nio.channels.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public class A20JvmRuntime {
    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("child")) {
            System.out.println("A20_JVM_CHILD");
            System.exit(7);
        }
        Path dir = Files.createTempDirectory("a20-jvm-");
        Path hook = dir.resolve("shutdown-hook");
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            try { Files.writeString(hook, "normal-exit\n"); }
            catch (IOException e) { throw new UncheckedIOException(e); }
        }));
        Path data = dir.resolve("mapped");
        try (FileChannel file = FileChannel.open(data, StandardOpenOption.CREATE,
                StandardOpenOption.READ, StandardOpenOption.WRITE)) {
            MappedByteBuffer map = file.map(FileChannel.MapMode.READ_WRITE, 0, 4096);
            map.putInt(0, 0x12345678);
            map.force();
            if (ByteBuffer.wrap(Files.readAllBytes(data)).getInt() != 0x12345678)
                throw new AssertionError("mapped writeback");
        }
        CountDownLatch start = new CountDownLatch(1);
        ExecutorService pool = Executors.newFixedThreadPool(4);
        List<Future<Integer>> workers = new ArrayList<>();
        for (int i = 0; i < 4; i++) {
            final int value = i;
            workers.add(pool.submit(() -> {
                ThreadLocal<Integer> local = new ThreadLocal<>();
                local.set(value);
                start.await();
                Thread.sleep(20);
                return local.get();
            }));
        }
        start.countDown();
        for (int i = 0; i < workers.size(); i++)
            if (workers.get(i).get(10, TimeUnit.SECONDS) != i)
                throw new AssertionError("thread-local state");
        pool.shutdown();
        if (!pool.awaitTermination(10, TimeUnit.SECONDS))
            throw new AssertionError("executor shutdown");
        String java = System.getProperty("java.home") + "/bin/java";
        Process child = new ProcessBuilder(java, "-cp", System.getProperty("java.class.path"),
                "A20JvmRuntime", "child").redirectErrorStream(true).start();
        String output = new String(child.getInputStream().readAllBytes());
        if (!child.waitFor(15, TimeUnit.SECONDS) || child.exitValue() != 7 ||
                !output.contains("A20_JVM_CHILD"))
            throw new AssertionError("JVM child: " + output);
        List<String> interfaces = new ArrayList<>();
        for (NetworkInterface iface : Collections.list(NetworkInterface.getNetworkInterfaces()))
            interfaces.add(iface.getName());
        if (interfaces.isEmpty()) throw new AssertionError("network interface enumeration");
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getLoopbackAddress());
                Socket client = new Socket(InetAddress.getLoopbackAddress(), server.getLocalPort());
                Socket peer = server.accept()) {
            peer.setSoTimeout(5000);
            client.getOutputStream().write(73);
            if (peer.getInputStream().read() != 73) throw new AssertionError("socket data");
        }
        if (args.length != 1) throw new IllegalArgumentException("native directory required");
        for (String library : new String[]{"liblwjgl.so", "libglfw.so"}) {
            System.load(Path.of(args[0], library).toAbsolutePath().toString());
            System.out.println("A20_JVM: loaded " + library);
        }
        System.out.println("A20_JVM: PASS mapped I/O, threads/TLS, subprocess, interfaces=" +
                interfaces + ", loopback socket, LWJGL/GLFW; hook=" + hook);
    }
}
