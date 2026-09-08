// Copyright 2019, 2026 Azul Systems, Inc.  All Rights Reserved.
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

import jdk.crac.CheckpointException;
import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.RestoreException;
import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.Utils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.security.SecureRandom;

/*
 * @test id=SHA1PRNG
 * @summary Verify that SHA1PRNG secure random is not interlocked during checkpoint/restore.
 * @library /test/lib
 * @build InterlockTest
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest SHA1PRNG
 */
/*
 * @test id=NativePRNGNonBlocking
 * @summary Verify that NativePRNGNonBlocking secure random is not interlocked during checkpoint/restore.
 * @requires (os.family != "windows")
 * @library /test/lib
 * @build InterlockTest
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest NativePRNGNonBlocking
 */
/*
 * @test id=NativePRNG
 * @summary Verify that NativePRNG secure random is not interlocked during checkpoint/restore.
 * @requires (os.family != "windows")
 * @library /test/lib
 * @build InterlockTest
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest NativePRNG
 */

/* NativePRNGBlocking is excluded as on some machines /dev/random is exhausted
 * too soon, making the test running too long. */

public class InterlockTest implements Resource, CracTest {
    private static final long SLEEP_MS = Utils.adjustTimeout(25);

    private volatile boolean stop = false;
    private SecureRandom sr;

    @CracTestArg
    String algName;

    private class TestThread1 extends Thread {
        @Override
        public void run() {
            while (!stop) {
                set();
            }
        }
    }

    private class TestThread2 extends Thread implements Resource {
        private final SecureRandom sr;

        synchronized void set() {
            sr.nextInt();
        }
        synchronized void clean() {
            sr.nextInt();
        }

        TestThread2() throws Exception {
            sr = SecureRandom.getInstance(algName);
            Context.getGlobalContext().register(this);
        }

        @Override
        public void run() {
            while (!stop) {
                set();
            }
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            clean();
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) throws Exception {
            set();
        }
    }

    synchronized void clean() {
        sr.nextInt();
    }

    synchronized void set() {
        sr.nextInt();
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        clean();
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        set();
        stop = true;
    }

    @Override
    public void test() throws Exception {
        new CracBuilder().engine(CracEngine.SIMULATE).doCheckpoint();
    }

    @Override
    public void exec() throws Exception {
        sr = SecureRandom.getInstance(algName);
        Context.getGlobalContext().register(this);

        try {
            final int numThreads = Math.min(4 * Runtime.getRuntime().availableProcessors(), 100);
            System.err.println("Spawning " + numThreads + " test threads");
            for (int i = 0; i < numThreads; i++) {
                final var testThread = (i % 2 == 0) ? new TestThread1(): new TestThread2();
                testThread.start();
            }
            Thread.sleep(SLEEP_MS);
            set();
            Thread.sleep(SLEEP_MS);

            final var checkpointThreadSucceeded = new boolean[1];
            final var checkpointThread = new Thread(() -> {
                try {
                    CRaCMXBean.getCRaCMXBean().checkpointRestore();
                    checkpointThreadSucceeded[0] = true;
                } catch (CheckpointException | RestoreException e) {
                    throw new RuntimeException("Checkpoint/restore failed", e);
                }
            }, "Checkpoint thread");
            checkpointThread.start();
            checkpointThread.join();
            if (!checkpointThreadSucceeded[0]) {
                throw new RuntimeException("Checkpoint thread failed");
            }

            Thread.sleep(10 * SLEEP_MS);
        } finally {
            stop = true; // Ensure non-daemon test threads don't block JVM from exiting
        }
    }
}
