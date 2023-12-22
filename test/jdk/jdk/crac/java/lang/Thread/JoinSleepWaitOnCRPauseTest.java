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

import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.util.concurrent.CountDownLatch;
/*
 * @test JoinSleepWaitOnCRPauseTest.java
 * @requires (os.family == "linux")
 * @library /test/lib
 * @summary check if Thread.join(timeout), Thread.sleep(timeout)
 *          and Object.wait(timeout)
 *          will be completed on restore immediately
 *          if their end time fell on the CRaC pause period
 *          (i.e. between the checkpoint and restore)
 *
 * @build JoinSleepWaitOnCRPauseTest
 * @run driver jdk.test.lib.crac.CracTest join_ms
 * @run driver jdk.test.lib.crac.CracTest join_ns
 * @run driver jdk.test.lib.crac.CracTest sleep_ms
 * @run driver jdk.test.lib.crac.CracTest sleep_ns
 * @run driver jdk.test.lib.crac.CracTest wait_ms
 * @run driver jdk.test.lib.crac.CracTest wait_ns
 */
public class JoinSleepWaitOnCRPauseTest implements CracTest {
    private enum TestType {
        join_ms, join_ns, sleep_ms, sleep_ns, wait_ms, wait_ns
    }

    @CracTestArg
    private TestType testType;

    private final static long EPS_NS = Long.parseLong(System.getProperty(
        "test.jdk.jdk.crac.java.lang.Thread.crac.JoinSleepWaitOnCRPauseTest.eps", getEnv("crac_JoinSleepWaitOnCRPauseTest_eps", "500000000"))); // default: 500ms

    private static final long CRPAUSE_MS = 40 * EPS_NS / 1_000_000;

    private static final long T_MS = CRPAUSE_MS / 2;
    private static final  int T_NS = 100;

    private volatile long tDone = -1;

    private final CountDownLatch checkpointLatch = new CountDownLatch(1);

    private static String getEnv(String name, String defaultValue) {
        String val = System.getenv(name);
        return val == null ?
                defaultValue :
                val;
    }

    @Override
    public void exec() throws Exception {

        String op = testType.name();
        op = op.substring(0, op.length() - 3); // remove suffix

        Thread mainThread = Thread.currentThread();

        Runnable r = () -> {

            try {

                checkpointLatch.countDown();

                switch (testType) {

                    case join_ms:
                        mainThread.join(T_MS);
                        break;

                    case join_ns:
                        mainThread.join(T_MS, T_NS);
                        break;

                    case sleep_ms:
                        Thread.sleep(T_MS);
                        break;

                    case sleep_ns:
                        Thread.sleep(T_MS, T_NS);
                        break;

                    case wait_ms:
                        synchronized(this) { wait(T_MS);  }
                        break;

                    case wait_ns:
                        synchronized(this) { wait(T_MS, T_NS);  }
                        break;

                    default:
                        throw new IllegalArgumentException("unknown test type");
                }

            } catch (InterruptedException ie) {
                throw new RuntimeException(ie);
            }

            tDone = System.nanoTime();
        };

        Thread t = new Thread(r);
        long tStart = System.nanoTime();
        t.start();

        // this additional synchronization is probably redundant;
        // adding it to ensure we get the expected TIMED_WAITING state
        // from "our" join or sleep
        checkpointLatch.await();

        // it is expected that EPS_NS is enough to complete the join/sleep on restore
        // so we are expecting that it should be enough to enter them
        Thread.sleep(EPS_NS / 1_000_000);

        if (t.getState() != Thread.State.TIMED_WAITING) {
            throw new AssertionError(String.format("The created thread was not able to enter %s in %s ns", op, EPS_NS));
        }

        long tBeforeCheckpoint = System.nanoTime();

        jdk.crac.Core.checkpointRestore();

        long tAfterRestore = System.nanoTime();

        t.join();

        System.out.println(String.format("The test started at %s", tStart));

        long pause = (tAfterRestore - tBeforeCheckpoint)/1_000_000;
        if (pause < CRPAUSE_MS) {
            throw new AssertionError(String.format("the CR pause %s ms was less than the expected pause %s ms", pause, CRPAUSE_MS));
        }

        if (tDone < tBeforeCheckpoint) {
            throw new AssertionError(String.format("%s has finished before the checkpoint at %s ms", op, tDone/1_000_000));
        }

        long eps = Math.abs(tAfterRestore - tDone);

        if (eps > EPS_NS) {
            throw new RuntimeException(String.format(
                "The %sing thread has finished at %s in %s ns "
                + "before/after the restore (expected was: %s ns)", op, tDone, eps, EPS_NS));
        }
    }

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder()
                .imageDir("cr_" + testType.name());
        builder.doCheckpoint();

        // sleep a few seconds to ensure the task execution time
        // falls within this pause period
        Thread.sleep(CRPAUSE_MS);

        builder.doRestore();
    }
}
