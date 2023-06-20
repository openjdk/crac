/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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

import jdk.test.lib.Container;
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.LockSupport;
import java.util.concurrent.locks.ReentrantLock;

import static jdk.test.lib.Asserts.*;

/**
 * @test TimedWaitingTest checks whether timed waiting does not block when monotonic time runs backwards
 * @requires (os.family == "linux")
 * @requires docker.support
 * @library /test/lib
 * @build TimedWaitingTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class TimedWaitingTest implements CracTest {
    private static final String imageName = Common.imageName("timed-waiting");
    public static final String WAITING = "WAITING";
    public static final int WAIT_TIME_MILLIS = 1000;

    @Override
    public void test() throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            return;
        }

        CracBuilder builder = new CracBuilder();
        Path bootIdFile = Files.createTempFile("NanoTimeTest-", "-boot_id");
        try {
            builder.withBaseImage("ghcr.io/crac/test-base", "latest")
                    .dockerOptions("-v", bootIdFile + ":/fake_boot_id")
                    .inDockerImage(imageName);
            builder.captureOutput(true);

            Files.writeString(bootIdFile, "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\n");
            // We need to preload the library before checkpoint
            CracProcess checkpointed = builder.startCheckpoint(Container.ENGINE_COMMAND, "exec",
                    "-e", "LD_PRELOAD=/opt/path-mapping-quiet.so",
                    "-e", "PATH_MAPPING=/proc/sys/kernel/random/boot_id:/fake_boot_id",
                    CracBuilder.CONTAINER_NAME,
                    "unshare", "--fork", "--time", "--monotonic", "86400", "--boottime", "86400",
                    CracBuilder.DOCKER_JAVA);
            CountDownLatch latch = new CountDownLatch(1);
            checkpointed.watch(out -> {
                System.out.println(out);
                if (WAITING.equals(out)) {
                    latch.countDown();
                }
            }, System.err::println);
            latch.await();
            builder.checkpointViaJcmd();
            checkpointed.waitForCheckpointed();

            Files.writeString(bootIdFile, "yyyyyyyy-yyyy-yyyy-yyyy-yyyyyyyyyyyy\n");

            CracProcess restore = builder.startRestore();
            CompletableFuture<Void> future = new CompletableFuture<>();
            new Thread(() -> {
                try {
                    restore.waitForSuccess();
                    System.err.print(restore.outputAnalyzer().getStderr());
                    future.complete(null);
                } catch (Throwable t) {
                    future.completeExceptionally(t);
                }
            }).start();
            future.get(10, TimeUnit.SECONDS);
        } finally {
            builder.ensureContainerKilled();
            assertTrue(bootIdFile.toFile().delete());
        }
    }

    private interface Task {
        void run() throws InterruptedException;
    }

    private static void timedWait(Task task, List<Throwable> exceptions, boolean canReturnEarly) {
        try {
            long before = System.currentTimeMillis();
            task.run();
            long after = System.currentTimeMillis();
            if (after - before < WAIT_TIME_MILLIS) {
                if (canReturnEarly) {
                    // Non-critical
                    System.err.println(Thread.currentThread().getName() + " took: " + (after - before) + " ms");
                } else {
                    exceptions.add(new IllegalStateException(
                            Thread.currentThread().getName() + " was too short: " + (after - before) + " ms"));
                }
            }
        } catch (InterruptedException e) {
            exceptions.add(unexpectedInterrupt(e));
        }
    }

    @Override
    public void exec() throws Exception {
        List<Throwable> exceptions = Collections.synchronizedList(new ArrayList<>());
        List<Thread> threads = new ArrayList<>();
        CountDownLatch latch = new CountDownLatch(6);

        startThread("Thread.sleep", threads, latch, () -> {
            timedWait(() -> Thread.sleep(WAIT_TIME_MILLIS), exceptions, false);
        });

        startThread("Thread.join", threads, latch, () -> {
            Thread daemon = new Thread(() -> {
                try {
                    Thread.sleep(86_400_000);
                } catch (InterruptedException e) {
                    exceptions.add(unexpectedInterrupt(e));
                }
            }, "inifinite daemon");
            daemon.setDaemon(true);
            daemon.start();
            timedWait(() -> daemon.join(WAIT_TIME_MILLIS), exceptions, false);
        });

        startThread("Object.wait", threads, latch, () -> {
            synchronized (this) {
                timedWait(() -> this.wait(WAIT_TIME_MILLIS), exceptions, true);
            }
        });

        ReentrantLock lock = new ReentrantLock();
        lock.lock();
        startThread("ReentrantLock.tryLock", threads, latch, () -> {
            timedWait(() -> {
                if (lock.tryLock(WAIT_TIME_MILLIS, TimeUnit.MILLISECONDS)) {
                    exceptions.add(new AssertionError("Should not be able to lock"));
                }
            }, exceptions, false);
        });

        startThread("Condition.await", threads, latch, () -> {
            ReentrantLock lock2 = new ReentrantLock();
            Condition condition = lock2.newCondition();
            lock2.lock();
            //noinspection ResultOfMethodCallIgnored
            timedWait(() -> condition.await(WAIT_TIME_MILLIS, TimeUnit.MILLISECONDS), exceptions, true);
        });

        startThread("LockSupport.parkUntil", threads, latch, () -> {
            timedWait(() -> LockSupport.parkUntil(System.currentTimeMillis() + WAIT_TIME_MILLIS),
                    exceptions, true);
        });

        assertEquals(latch.getCount(), (long) threads.size());
        do {
            Thread.yield();
            threads.stream().forEach(t -> {
                System.out.printf("%s: %s%n", t.getName(), t.getState());
            });
        } while (!threads.stream().map(Thread::getState).allMatch(Thread.State.TIMED_WAITING::equals));
        System.out.println(WAITING);
        // Make sure none of the threads completed yet
        assertEquals(latch.getCount(), (long) threads.size());
        try {
            latch.await();
        } catch (InterruptedException e) {
            fail("Should not get interrupted", e);
        }
        assertEquals(Collections.emptyList(), exceptions);
    }

    private static void startThread(String name, List<Thread> threads, CountDownLatch latch, Runnable runnable) {
        Thread thread = new Thread(() -> {
            try {
                runnable.run();
            } finally {
                latch.countDown();
            }
        }, name);
        threads.add(thread);
        thread.start();
    }

    private static AssertionError unexpectedInterrupt(InterruptedException e) {
        return new AssertionError(Thread.currentThread().getName() + " interrupted", e);
    }
}
