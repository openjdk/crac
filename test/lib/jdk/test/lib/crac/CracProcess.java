/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package jdk.test.lib.crac;

import jdk.test.lib.Utils;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.StreamPumper;

import java.io.BufferedReader;
import java.io.Closeable;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.file.*;
import java.util.ArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.List;
import java.util.concurrent.TimeoutException;
import java.util.function.Predicate;
import java.util.stream.Stream;

import static jdk.test.lib.Asserts.*;

public class CracProcess implements Closeable {
    private static final String PAUSE_PID_FILE = "pid";

    private final Process process;

    // Saved from CracBuilderBase because that can be modified
    private final CracEngine engine;
    private final Path imageDir;

    private final StreamProcessor stdoutProcessor;
    private final StreamProcessor stderrProcessor;

    private static class StreamProcessor implements AutoCloseable {
        private final Future<Void> future;
        private final List<String> lines = new ArrayList<>();
        private int lineWaitIndex = 0;

        StreamProcessor(InputStream is, String logPrefix) {
            future = new StreamPumper(is).addPump(new StreamPumper.LinePump() {
                @Override
                protected void processLine(String line) {
                    CracBuilderBase.log("%s %s", logPrefix, line);
                    synchronized (lines) {
                        lines.add(line);
                        lines.notifyAll();
                    }
                }
            }).process();
        }

        @Override
        public void close() {
            future.cancel(true);
        }

        void waitForLine(Predicate<String> predicate, int start, long timeout, TimeUnit unit) throws InterruptedException, EOFException, TimeoutException {
            final var nanoTimeThreshold = System.nanoTime() + unit.toNanos(timeout);
            synchronized (lines) {
                if (start >= 0) {
                    lineWaitIndex = start;
                }
                while (!future.isDone() && System.nanoTime() < nanoTimeThreshold) {
                    if (lineWaitIndex < lines.size()) {
                        final var iter = lines.listIterator(lineWaitIndex);
                        while (iter.hasNext()) {
                            lineWaitIndex++; // Next time continue from the next line
                            if (predicate.test(iter.next())) {
                                return;
                            }
                        }
                    }
                    final var waitTime = TimeUnit.NANOSECONDS.toMillis(nanoTimeThreshold - System.nanoTime());
                    if (waitTime > 0) {
                        lines.wait(waitTime);
                    }
                }
            }
            if (future.isDone()) {
                throw new EOFException("Process finished before printing required line");
            }
            throw new TimeoutException("Required line was not printed in time");
        }

        List<String> waitForAllLines() throws InterruptedException {
            try {
                future.get();
            } catch (ExecutionException e) {
                throw new RuntimeException(e);
            }
            return lines;
        }
    }

    public CracProcess(CracBuilderBase<?> builder, List<String> cmd) throws IOException {
        engine = builder.engine;
        imageDir = builder.imageDir != null ? builder.imageDir() : null;

        ProcessBuilder pb = new ProcessBuilder();
        pb.environment().putAll(builder.env);
        process = pb.command(cmd).start();
        CracBuilderBase.log("Started process " + process.pid());

        stdoutProcessor = new StreamProcessor(process.getInputStream(), "[" + process.pid() + " OUT]");
        stderrProcessor = new StreamProcessor(process.getErrorStream(), "[" + process.pid() + " ERR]");
    }

    @Override
    public void close() throws IOException {
        if (stdoutProcessor != null) {
            stdoutProcessor.close();
            stderrProcessor.close();
        }
        process.destroyForcibly();
        process.close();
    }

    public int waitFor() throws InterruptedException {
        return process.waitFor();
    }

    public void waitForCheckpointed() throws InterruptedException {
        if (engine == null || engine == CracEngine.CRIU) {
            final var exitValue = process.waitFor();
            assertEquals(137, exitValue, "Checkpointed process was not killed as expected.");
            CracBuilderBase.log("Process %d completed with exit value %d", process.pid(), exitValue);
        } else {
            fail("With engine " + engine.engine + " use the async version.");
        }
    }

    public void waitForPausePid() throws IOException, InterruptedException {
        assertEquals(CracEngine.PAUSE, engine, "Pause PID file is created only with pauseengine");

        // (at least on Windows) we need to wait to avoid os::prepare_checkpoint() interference with mkdir/rmdir calls
        Thread.sleep(500);

        try (WatchService watcher = FileSystems.getDefault().newWatchService()) {
            Path absImageDir = imageDir.toAbsolutePath();
            waitForFileCreated(watcher, absImageDir.getParent(), path -> path.getFileName().equals(imageDir.getFileName()));
            waitForFileCreated(watcher, absImageDir, path -> path.getFileName().toString().equals(PAUSE_PID_FILE));
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

    public void waitForSuccess() throws InterruptedException {
        int exitValue = process.waitFor();
        assertEquals(0, exitValue, "Process returned unexpected exit code: " + exitValue);
        CracBuilderBase.log("Process %d completed with exit value %d", process.pid(), exitValue);
    }

    public void waitForStdout(Predicate<String> predicate, long timeoutSec) throws InterruptedException, EOFException, TimeoutException {
        stdoutProcessor.waitForLine(predicate, -1, timeoutSec, TimeUnit.SECONDS);
    }

    public void waitForStdout(String str, boolean failOnUnexpected) throws InterruptedException {
        final var timeoutSec = (long) (10 * Utils.TIMEOUT_FACTOR);
        try {
            waitForStdout((line) -> {
                if (line.equals(str)) {
                    return true;
                } else if (failOnUnexpected) {
                    fail("Unexpected stdout of process " + pid() + ": '" + line + "' - does not contain '" + str + "'");
                }
                return false;
            }, timeoutSec);
        } catch (EOFException e) {
            fail("Unexpected stdout of process " + pid() + ": exited before printing '" + str + "'");
        } catch (TimeoutException e) {
            fail("Timeout " + timeoutSec + "s waiting for stdout of process " + pid() + " to produce '" + str + "' - you can use TIMEOUT_FACTOR to change the timeout");
        }
    }

    /**
     * Waits for the process to finish and returns {@link OutputAnalyzer} for its output.
     */
    public OutputAnalyzer outputAnalyzer() throws InterruptedException {
        // Cannot just construct OutputAnalyzer from the process because we are reading its streams
        // and thus OutputAnalyzer won't be able to get their contents
        final var exitCode = waitFor();
        return new OutputAnalyzer(
                String.join(System.lineSeparator(), stdoutProcessor.waitForAllLines()),
                String.join(System.lineSeparator(), stderrProcessor.waitForAllLines()),
                exitCode
        );
    }

    public long pid() {
        return process.pid();
    }

    public OutputStream input() {
        return process.getOutputStream();
    }

    public void sendNewline() throws IOException {
        OutputStream input = process.getOutputStream();
        input.write('\n');
        input.flush();
    }

    public void destroyForcibly() {
        process.destroyForcibly();
    }

    public void printThreadDump() throws IOException {
        final long pid = this.pid();
        boolean isAlive = ProcessHandle.of(pid).map(ProcessHandle::isAlive).orElse(false);
        if (!isAlive) {
            CracBuilderBase.log("Cannot print thread dump: process " + pid + " is not alive");
        } else {
            CracBuilderBase.log("Running: jcmd " + pid + " Thread.print");
            Process jcmdProc = new ProcessBuilder(jdk.test.lib.Utils.TEST_JDK + "/bin/jcmd", String.valueOf(pid), "Thread.print")
                    .redirectErrorStream(true)
                    .start();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(jcmdProc.getInputStream()))) {
                for (String line = reader.readLine(); null != line; line = reader.readLine()) {
                    CracBuilderBase.log("[JCMD for " + pid + "] " + line);
                }
            }
        }
    }

    private static boolean checkGcoreAvailable() {
        ProcessBuilder builder = new ProcessBuilder("which", "gcore");
        builder.redirectErrorStream(true);
        try (var checker = builder.start()) {
            BufferedReader reader = new BufferedReader(new InputStreamReader(checker.getInputStream()));
            String line = reader.readLine();
            int exitCode = checker.waitFor();
            if (exitCode == 0 && line != null && !line.trim().isEmpty()) {
                CracBuilderBase.log("gcore is available.");
                return true;
            } else {
                CracBuilderBase.log("gcore is NOT available.");
                return false;
            }
        } catch (IOException | InterruptedException e) {
            CracBuilderBase.log("Could not run 'which gcore' or was interrupted");
            return false;
        }
    }

    public void dumpProcess() {
        // For gcore, it's required 'sudo sysctl -w kernel.yama.ptrace_scope=0'
        // For kill, it's required 'ulimit -c unlimited && echo core.%p | sudo tee /proc/sys/kernel/core_pattern'
        final long pid = this.pid();
        ProcessBuilder builder = checkGcoreAvailable() ? new ProcessBuilder("gcore", String.valueOf(pid))
                : new ProcessBuilder("kill", "-ABRT", String.valueOf(pid));
        builder.redirectErrorStream(true);
        try (var dumper = builder.start();) {
            var reader = new AsyncStreamReader(dumper.getInputStream());
            int exitCode = dumper.waitFor();
            try {
                while (true) {
                    CracBuilderBase.log("dumpProcess: " + reader.readLine(100));
                }
            } catch (Exception e) {
                // do nothing
            }
            if (exitCode == 0) {
                CracBuilderBase.log("Core dump seems to be created successfully for pid=" + pid);
            } else {
                CracBuilderBase.log("Something went wrong while dumping pid=" + pid);
            }
        } catch (IOException | InterruptedException e) {
            CracBuilderBase.log("Exception thrown while dumping pid=" + pid);
            e.printStackTrace();
        }
    }

    public void clearPausePid() throws IOException {
        assertEquals(CracEngine.PAUSE, engine, "Pause PID file is created only with pauseengine");
        Files.delete(imageDir.resolve(PAUSE_PID_FILE));
    }
}
