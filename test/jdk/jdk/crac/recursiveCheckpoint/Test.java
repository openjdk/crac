// Copyright 2019-2021 Azul Systems, Inc.  All Rights Reserved.
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

import jdk.crac.*;
import jdk.test.lib.crac.*;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicInteger;

/*
 * @test
 * @summary check that the recursive checkpoint is not allowed
 * @library /test/lib
 * @build Test
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest 10
 */
public class Test implements Resource, CracTest {
    private static final AtomicInteger counter = new AtomicInteger(0);
    private static final AtomicInteger restoreCount = new AtomicInteger(0);
    private static Exception exception = null;

    @CracTestArg(0)
    int numThreads;

    @CracTestArg(value = 1, optional = true)
    String restoreCountPath;

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.PAUSE);
        // remove pid file if it exists from previous run
        builder.imageDir().resolve("pid").toFile().delete();
        restoreCountPath = builder.imageDir().resolve("restoreCount").toAbsolutePath().toString();
        builder.imageDir().toFile().mkdirs();
        Files.writeString(Path.of(restoreCountPath), "0");

        builder.args(CracTest.args(restoreCountPath));
        CracProcess process = builder.startCheckpoint();
        process.waitForPausePid();
        for (int i = 1; i <= numThreads + 1; ++i) {
            assert process.isAlive();
            System.err.printf("Restore #%d%n", i);
            builder.doRestore();
            String currentRestore;
            do {
                Thread.sleep(20);
                currentRestore = Files.readString(Path.of(restoreCountPath));
            } while(!currentRestore.equals(String.valueOf(i)));
        }
        process.waitForSuccess();
    }

    private class TestThread extends Thread {

        @Override
        public void run() {
            try {
                doCheckpointRestore();
            } catch (CheckpointException e) {
                if (exception == null)
                    exception = new RuntimeException("Checkpoint in thread ERROR " + e);
            } catch (RestoreException e) {
                if (exception == null)
                    exception = new RuntimeException("Restore in thread ERROR " + e);
            } catch (IOException e) {
                if (exception == null) {
                    exception = e;
                }
            }
        }
    };

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        try {
            int c = counter.incrementAndGet();
            if (c > 1) {
                if (exception == null)
                    exception = new RuntimeException("Parallel checkpoint");
            }
            Thread.sleep(100);
            jdk.crac.Core.checkpointRestore();
            if (exception != null)
                exception = new RuntimeException("Checkpoint Exception should be thrown");
        } catch (CheckpointException e) {
            // Expected Exception
        } catch (RestoreException e) {
            if (exception == null)
                exception = new RuntimeException("Restore ERROR " + e);
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        try {
            int c = counter.get();
            if (c > 1) {
                if (exception == null)
                    exception = new RuntimeException("Parallel checkpoint");
            }
            Thread.sleep(100);
            jdk.crac.Core.checkpointRestore();
            if (exception == null)
                exception = new RuntimeException("Checkpoint Exception should be thrown");
        } catch (CheckpointException e) {
            // Expected Exception
        } catch (RestoreException e) {
            if (exception == null)
                exception = new RuntimeException("Restore ERROR " + e);
        } finally {
            counter.decrementAndGet();
        }
    }

    @Override
    public void exec() throws Exception {
        Core.getGlobalContext().register(this);

        TestThread[] threads = new TestThread[numThreads];
        for (int i = 0; i < numThreads; i++) {
            threads[i] = new TestThread();
            threads[i].start();
        };

        Thread.sleep(100);
        try {
            doCheckpointRestore();
        } catch (CheckpointException e) {
            throw new RuntimeException("Checkpoint ERROR", e);
        } catch (RestoreException e) {
            throw new RuntimeException("Restore ERROR", e);
        }

        for (int i = 0; i < numThreads; i++) {
            threads[i].join();
        };

        long ccounter = counter.get();
        if (ccounter != 0)
            throw new RuntimeException("Incorrect counter after restore: " + ccounter + " instead of 0");
        if (exception != null) {
            throw exception;
        }
        System.out.println("PASSED");
    }

    private void doCheckpointRestore() throws CheckpointException, RestoreException, IOException {
        Core.checkpointRestore();
        int rc = restoreCount.incrementAndGet();
        System.err.printf("After restore #%d%n", rc, restoreCountPath);
        Files.writeString(Path.of(restoreCountPath), String.valueOf(rc));
    }
}
