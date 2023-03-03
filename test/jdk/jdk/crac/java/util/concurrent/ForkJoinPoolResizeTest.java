/*
 * Copyright (c) 2022, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.Core;
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;
import jdk.test.lib.process.ProcessTools;

import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @build ForkJoinPoolResizeTest
 * @run driver jdk.test.lib.crac.CracTest 1 4
 * @run driver jdk.test.lib.crac.CracTest 8 16
 * @run driver jdk.test.lib.crac.CracTest 4 1
 * @run driver jdk.test.lib.crac.CracTest 16 1
 * @run driver jdk.test.lib.crac.CracTest 1 1
 * @run driver jdk.test.lib.crac.CracTest 4 4
 */
public class ForkJoinPoolResizeTest implements CracTest {
    public static final String IMAGE_NAME = Common.imageName("fork-join-pool");
    public static final long KEEP_ALIVE_TIME = 100; // milliseconds
    @CracTestArg(0)
    int initialCpus;

    @CracTestArg(1)
    int restoreCpus;

    @Override
    public void test() throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            System.err.println("Docker not available");
            return;
        }
        int cpus = Runtime.getRuntime().availableProcessors();
        if (initialCpus > cpus || restoreCpus > cpus) {
            System.err.printf("Ignoring test that requires %d CPUs but we have at most %d%n", Math.max(initialCpus, restoreCpus), cpus);
            return;
        }
        CracBuilder builder = new CracBuilder().inDockerImage(IMAGE_NAME);
        builder.dockerOptions("--cpus", String.valueOf(initialCpus)).doCheckpoint();
        builder.recreateContainer(IMAGE_NAME, "--cpus", String.valueOf(restoreCpus));
        builder.doRestore();

    }

    @Override
    public void exec() throws Exception {
        assertEquals(initialCpus, Runtime.getRuntime().availableProcessors());
        AtomicInteger threadCounter = new AtomicInteger(0);
        ForkJoinPool fjp = new ForkJoinPool(initialCpus, pool -> {
            threadCounter.incrementAndGet();
            return ForkJoinPool.defaultForkJoinWorkerThreadFactory.newThread(pool);
        }, null, false,
                initialCpus, 100, 1, null, KEEP_ALIVE_TIME, TimeUnit.MILLISECONDS);
        assertEquals(0, fjp.getPoolSize());
        assertEquals(initialCpus, fjp.getParallelism());

        CountDownLatch firstBatchStart = new CountDownLatch(initialCpus);
        // We use twice as many jobs to make sure not more than expected threads are created
        CountDownLatch firstBatchEnd = new CountDownLatch(initialCpus * 2);
        startJobs(fjp, firstBatchStart, firstBatchEnd, initialCpus * 2);
        firstBatchStart.await();
        assertEquals(initialCpus, fjp.getActiveThreadCount());
        firstBatchEnd.await();
        assertEquals(initialCpus, fjp.getPoolSize());

        // If we wait > initialCpus * keep alive the threads will die
        Thread.sleep((initialCpus + 1) * KEEP_ALIVE_TIME);
        assertEquals(0, fjp.getActiveThreadCount());
        assertEquals(0, fjp.getPoolSize());
        threadCounter.set(0);

        Core.checkpointRestore();
        assertEquals(restoreCpus, Runtime.getRuntime().availableProcessors());
        assertEquals(restoreCpus, fjp.getParallelism());

        CountDownLatch secondBatchStart = new CountDownLatch(restoreCpus);
        CountDownLatch thirdBatchEnd = new CountDownLatch(restoreCpus);
        // Scheduling jobs does not immediately start all the threads; we have to wait for them to start
        startJobs(fjp, secondBatchStart, null, restoreCpus);
        startJobs(fjp, null, thirdBatchEnd, restoreCpus);
        secondBatchStart.await();

        assertEquals(restoreCpus, fjp.getPoolSize());
        // The active thread count is somewhat unreliable
//        assertEquals(restoreCpus, fjp.getActiveThreadCount());
        assertEquals(restoreCpus, threadCounter.get());

        thirdBatchEnd.await();
        // This should let any extra threads die off
        Thread.sleep((restoreCpus + 1) * KEEP_ALIVE_TIME);
        assertEquals(0, fjp.getPoolSize());
        assertEquals(0, fjp.getActiveThreadCount());
    }

    private static void startJobs(ForkJoinPool fjp, CountDownLatch startLatch, CountDownLatch completeLatch, int count) {
        for (int i = 0; i < count; ++i) {
            fjp.execute(() -> {
                if (startLatch != null) {
                    startLatch.countDown();
                }
                System.out.println(Thread.currentThread().getName() + " " + System.currentTimeMillis() + " START");
                try {
                    Thread.sleep(100);
                } catch (InterruptedException e) {
                    throw new RuntimeException(e);
                }
                if (completeLatch != null) {
                    completeLatch.countDown();
                }
                System.out.println(Thread.currentThread().getName() + " " + System.currentTimeMillis() + " COMPLETE ");
            });
        }
    }
}
