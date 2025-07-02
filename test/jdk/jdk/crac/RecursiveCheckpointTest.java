// Copyright 2019, 2025, Azul Systems, Inc.  All Rights Reserved.
// DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
//
// This code is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License version 2 only, as published by
// the Free Software Foundation.
//
// This code is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE.  See the GNU General Public License version 2 for more
// details (a copy is included in the LICENSE file that accompanied this code).
//
// You should have received a copy of the GNU General Public License version 2
// along with this work; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
//
// Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale,
// CA 94089 USA or visit www.azul.com if you need additional information or
// have any questions.

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import jdk.crac.*;
import jdk.test.lib.crac.*;
import static jdk.test.lib.Asserts.*;

import java.util.concurrent.atomic.AtomicInteger;

/*
 * @test
 * @summary check that the recursive checkpoint is not allowed
 * @library /test/lib
 * @build RecursiveCheckpointTest
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest 10
 */
public class RecursiveCheckpointTest implements Resource, CracTest {
    private static final AtomicInteger counter = new AtomicInteger(0);
    private static final List<Throwable> throwables = Collections.synchronizedList(new ArrayList<>());

    @CracTestArg
    int numThreads;

    @Override
    public void test() throws Exception {
        final CracBuilder builder = new CracBuilder().engine(CracEngine.SIMULATE);
        builder.startCheckpoint().waitForSuccess();
    }

    private static class TestThread extends Thread {
        public TestThread() {
            setUncaughtExceptionHandler(TestThread::handleException);
        }

        private static void handleException(@SuppressWarnings("unused") Thread thread, Throwable throwable) {
            throwables.add(throwable);
        }

        @Override
        public void run() {
            try {
                Core.checkpointRestore();
            } catch (CheckpointException | RestoreException e) {
                throw new IllegalStateException("C/R failed", e);
            }
        }
    };

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        assertEquals(1, counter.incrementAndGet(), "Concurrent checkpoint detected");
        Thread.sleep(100);
        try {
            Core.checkpointRestore();
            fail("Recursive checkpoint should fail");
        } catch (CheckpointException e) {
            // Expected exception
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        try {
            Thread.sleep(100);
            try {
                Core.checkpointRestore();
                fail("Recursive checkpoint should fail");
            } catch (CheckpointException e) {
                // Expected exception
            }
        } finally {
            counter.decrementAndGet();
        }
    }

    @Override
    public void exec() throws Exception {
        Core.getGlobalContext().register(new RecursiveCheckpointTest());

        final var threads = new TestThread[numThreads];
        for (int i = 0; i < numThreads; i++) {
            threads[i] = new TestThread();
            threads[i].start();
        }
        for (int i = 0; i < numThreads; i++) {
            threads[i].join();
        }

        if (!throwables.isEmpty()) {
            final var aggregated = new IllegalStateException("" + throwables.size() + " test threads failed");
            for (final var t : throwables) {
                aggregated.addSuppressed(t);
            }
            throw aggregated;
        }

        assertEquals(0, counter.get());
    }
}
