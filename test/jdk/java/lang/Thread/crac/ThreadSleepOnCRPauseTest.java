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

/*
 * @test ThreadSleepOnCRPauseTest.java
 * @requires (os.family == "linux")
 * @library /test/lib
 * @summary check if the Thread.sleep() will be completed on restore immediately
 *          if its end time fell on the CRaC pause period
 *          (i.e. between the checkpoint and restore)
 * @run main/othervm ThreadSleepOnCRPauseTest
 */


import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.util.concurrent.CountDownLatch;


public class ThreadSleepOnCRPauseTest {

    private final static long SLEEP_MS   = 1000;
    private final static int  SLEEP_NS   = 50;

    private final static long CRPAUSE_MS = 3000;

    private final static long EPS_NS = Long.parseLong(
        System.getProperty("test.time.eps", "100000000")); // default: 0.1s

    private final CountDownLatch sleepLatch = new CountDownLatch(1);

    private volatile long endTime = 0;


    private void runTest(boolean nsAccuracy) throws Exception {

        Runnable r = () -> {

            try {

                sleepLatch.countDown();

                if (nsAccuracy) {
                    Thread.sleep(SLEEP_MS, SLEEP_NS);
                } else {
                    Thread.sleep(SLEEP_MS);
                }

                endTime = System.nanoTime();

            } catch (InterruptedException ie) {
                throw new RuntimeException(ie);
            }
        };

        Thread t = new Thread(r);
        t.start();
        sleepLatch.await();

        while (t.getState() != Thread.State.TIMED_WAITING) {
            Thread.onSpinWait();
        }

        long beforeCheckpoint = System.nanoTime();

        jdk.crac.Core.checkpointRestore();

        long afterRestore = System.nanoTime();

        t.join();

        long pause = afterRestore - beforeCheckpoint;
        if (pause < 1_000_000 * CRPAUSE_MS - EPS_NS) {
            throw new RuntimeException(
                "the CR pause was less than " + CRPAUSE_MS + " ms");
        }

        if (endTime < beforeCheckpoint + EPS_NS) {
            throw new RuntimeException(
                "sleep has finished before the checkpoint");
        }

        long eps = Math.abs(afterRestore - endTime);

        if (eps > EPS_NS) {
            throw new RuntimeException("the sleeping thread has finished in " +
                eps + " ns after the restore (expected: " + EPS_NS + " ns)");
        }
    }


    public static void main(String args[]) throws Exception {

        if (args.length > 0) {

            new ThreadSleepOnCRPauseTest().runTest(Boolean.parseBoolean(args[0]));

        } else {

            ProcessBuilder pb;
            OutputAnalyzer out;

            String nsAccuracy[] = {"true", "false"};
            for (String arg: nsAccuracy) {

                pb = ProcessTools.createJavaProcessBuilder(
                    "-XX:CRaCCheckpointTo=cr", "ThreadSleepOnCRPauseTest", arg);
                out = new OutputAnalyzer(pb.start());
                out.shouldContain("CR: Checkpoint");
                out.shouldHaveExitValue(137);

                // sleep a few seconds to ensure the task execution time
                // falls within this pause period
                Thread.sleep(CRPAUSE_MS);

                pb = ProcessTools.createJavaProcessBuilder(
                    "-XX:CRaCRestoreFrom=cr", "ThreadSleepOnCRPauseTest", arg);
                out = new OutputAnalyzer(pb.start());
                out.shouldHaveExitValue(0);

            }
        }
    }
}
