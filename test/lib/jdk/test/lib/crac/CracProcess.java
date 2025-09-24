package jdk.test.lib.crac;

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.StreamPumper;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.file.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import java.util.function.Predicate;
import java.util.stream.Stream;

import static jdk.test.lib.Asserts.*;

public class CracProcess {
    private final CracBuilder builder;
    private final Process process;

    public CracProcess(CracBuilder builder, List<String> cmd) throws IOException {
        this.builder = builder;
        ProcessBuilder pb = new ProcessBuilder().inheritIO().redirectInput(ProcessBuilder.Redirect.PIPE);
        if (builder.captureOutput) {
            pb.redirectOutput(ProcessBuilder.Redirect.PIPE);
            pb.redirectError(ProcessBuilder.Redirect.PIPE);
        }
        pb.environment().putAll(builder.env);
        this.process = pb.command(cmd).start();
    }

    public int waitFor() throws InterruptedException {
        return process.waitFor();
    }

    public void waitForCheckpointed() throws InterruptedException {
        if (builder.engine == null || builder.engine == CracEngine.CRIU) {
            final var exitValue = process.waitFor();
            if (exitValue != 137 && builder.captureOutput) {
                printOutput();
            }
            assertEquals(137, exitValue, "Checkpointed process was not killed as expected.");
            builder.log("Process %d completed with exit value %d%n", process.pid(), exitValue);
        } else {
            fail("With engine " + builder.engine.engine + " use the async version.");
        }
    }

    public void waitForPausePid() throws IOException, InterruptedException {
        assertEquals(CracEngine.PAUSE, builder.engine, "Pause PID file created only with pauseengine");

        // (at least on Windows) we need to wait to avoid os::prepare_checkpoint() interference with mkdir/rmdir calls
        Thread.sleep(500);

        try (WatchService watcher = FileSystems.getDefault().newWatchService()) {
            Path imageDir = builder.imageDir().toAbsolutePath();
            waitForFileCreated(watcher, imageDir.getParent(), path -> "cr".equals(path.toFile().getName()));
            waitForFileCreated(watcher, imageDir, path -> "pid".equals(path.toFile().getName()));
        }
    }

    private void waitForFileCreated(WatchService watcher, Path dir, Predicate<Path> predicate) throws IOException, InterruptedException {
        WatchKey key = dir.register(watcher, StandardWatchEventKinds.ENTRY_CREATE);
        assertTrue(key.isValid());
        try {
            try (Stream<Path> dirContents = Files.list(dir)) {
                if (dirContents.anyMatch(predicate)) {
                    // file already present
                    return;
                }
            }
            int timeoutCounter = 10;
            for (; ; ) {
                WatchKey key2 = watcher.poll(1, TimeUnit.SECONDS);
                if (null == key2) {
                    if (!process.isAlive() && 0 < --timeoutCounter) {
                        // At least on macOS, it seems like WatchService's event may be delayed up to 10 secs,
                        // so we need to keep waiting some time for the event, even the process is completed.
                        continue;
                    }
                    assertTrue(process.isAlive(), "Process should exist");
                    continue;
                }
                for (WatchEvent<?> event : key2.pollEvents()) {
                    if (event.kind() != StandardWatchEventKinds.ENTRY_CREATE) {
                        continue;
                    }
                    if (predicate.test((Path) event.context())) {
                        return;
                    }
                }
                key2.reset();
            }
        } finally {
            key.cancel();
        }
    }

    public CracProcess waitForSuccess() throws InterruptedException {
        int exitValue = process.waitFor();
        if (exitValue != 0 && builder.captureOutput) {
            printOutput();
        }
        assertEquals(0, exitValue, "Process returned unexpected exit code: " + exitValue);
        builder.log("Process %d completed with exit value %d%n", process.pid(), exitValue);
        return this;
    }

    private void printOutput() {
        final OutputAnalyzer oa;
        try {
            oa = outputAnalyzer();
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
        // Similar to OutputAnalyzer.reportDiagnosticSummary() but a bit better formatted
        System.err.println("stdout: [");
        System.err.print(oa.getStdout());
        System.err.println("]\nstderr: [");
        System.err.print(oa.getStderr());
        System.err.println("]\nexitValue = " + oa.getExitValue() + "\n");
    }

    public OutputAnalyzer outputAnalyzer() throws IOException {
        assertTrue(builder.captureOutput, "Output must be captured with .captureOutput(true)");
        return new OutputAnalyzer(process);
    }

    public CracProcess watch(Consumer<String> outputConsumer, Consumer<String> errorConsumer) {
        assertTrue(builder.captureOutput, "Output must be captured with .captureOutput(true)");
        pump(process.getInputStream(), outputConsumer);
        pump(process.getErrorStream(), errorConsumer);
        return this;
    }

    public void waitForStdout(String str) throws InterruptedException {
        waitForStdout(str, true);
    }

    public void waitForStdout(String str, boolean failOnUnexpected) throws InterruptedException {
        CountDownLatch latch = new CountDownLatch(1);
        AtomicReference<String> unexpected = new AtomicReference<>();
        watch(line -> {
            if (line.equals(str)) {
                latch.countDown();
            } else if (failOnUnexpected) {
                unexpected.set(line);
                latch.countDown();
            }
        }, System.err::println);
        assertTrue(latch.await(10, TimeUnit.SECONDS));
        String unexpectedLine = unexpected.get();
        if (unexpectedLine != null) {
            throw new IllegalArgumentException(unexpectedLine);
        }
    }

    private static void pump(InputStream stream, Consumer<String> consumer) {
        new StreamPumper(stream).addPump(new StreamPumper.LinePump() {
            @Override
            protected void processLine(String line) {
                consumer.accept(line);
            }
        }).process();
    }

    public long pid() {
        return process.pid();
    }

    public OutputStream input() {
        return process.getOutputStream();
    }

    public InputStream output() {
        return process.getInputStream();
    }

    public InputStream errOutput() {
        return process.getErrorStream();
    }

    public void sendNewline() throws IOException {
        OutputStream input = process.getOutputStream();
        input.write('\n');
        input.flush();
    }

    public void destroyForcibly() {
        process.destroyForcibly();
    }

    public static void printThreadDump(long pid) throws IOException {
        boolean isAlive = ProcessHandle.of(pid).map(ProcessHandle::isAlive).orElse(false);
        if (!isAlive) {
            System.err.println("Process " + pid + " is not alive.");
        } else {
            System.err.println("Running: jcmd " + pid + " Thread.print");
            Process jcmdProc = new ProcessBuilder(jdk.test.lib.Utils.TEST_JDK + "/bin/jcmd", String.valueOf(pid), "Thread.print")
                    .redirectErrorStream(true)
                    .start();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(jcmdProc.getInputStream()))) {
                for (String line = reader.readLine(); null != line; line = reader.readLine()) {
                    System.err.println("JCMD: " + line);
                }
            }
        }
    }

    private static boolean checkGcoreAvailable() {
        ProcessBuilder builder = new ProcessBuilder("which", "gcore");
        builder.redirectErrorStream(true);
        try {
            Process process = builder.start();
            BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()));
            String line = reader.readLine();
            int exitCode = process.waitFor();
            if (exitCode == 0 && line != null && !line.trim().isEmpty()) {
                System.out.println("gcore is available.");
                return true;
            } else {
                System.out.println("gcore is NOT available.");
                return false;
            }
        } catch (IOException | InterruptedException e) {
            System.out.println("Could not run 'which gcore' or was interrupted");
            return false;
        }
    }

    public static void dumpProcess(long pid) throws IOException, InterruptedException {
        // For gcore, it's required 'sudo sysctl -w kernel.yama.ptrace_scope=0'
        // For kill, it's required 'ulimit -c unlimited && echo core.%p | sudo tee /proc/sys/kernel/core_pattern'
        ProcessBuilder builder = checkGcoreAvailable() ? new ProcessBuilder("gcore", String.valueOf(pid))
                : new ProcessBuilder("kill", "-ABRT", String.valueOf(pid));
        builder.redirectErrorStream(true);
        try {
            Process process = builder.start();
            var reader = new AsyncStreamReader(process.getInputStream());
            int exitCode = process.waitFor();
            try {
                while (true) {
                    System.out.println("dumpProcess: " + reader.readLine(100));
                }
            } catch (Exception e) {
                // do nothing
            }
            if (exitCode == 0) {
                System.out.println("Core dump seems created successfully for pid=" + pid);
            } else {
                System.out.println("Something went wrong while dumping the app");
            }
        } catch (IOException | InterruptedException e) {
            System.out.println("Exception thrown while dumping the app");
            e.printStackTrace();
        }
    }

}
