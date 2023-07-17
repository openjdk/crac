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

import jdk.crac.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import static jdk.test.lib.Asserts.assertFalse;

/**
 * @test
 * @summary All afterRestore's should complete, even if there are only daemon threads (in case one of the afterRestore's finally creates non-daemon thread that will be responsible to keep VM alive)
 * @library /test/lib
 * @build DaemonAfterRestore
 * @run driver jdk.test.lib.crac.CracTest
 * @requires (os.family == "linux")
 */
public class DaemonAfterRestore implements CracTest {
    static final String MAIN_THREAD_FINISH = "main thread finish";
    static final String AFTER_RESTORE_MESSAGE = "after restore finish";

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().captureOutput(true);

        CompletableFuture<?> firstOutputFuture = new CompletableFuture<Void>();
        CracProcess checkpointProcess = builder.startCheckpoint().watch(
            outline -> {
                System.out.println(outline);
                if (outline.equals(MAIN_THREAD_FINISH)) {
                    firstOutputFuture.complete(null);
                }
            },
            errline -> {
                System.err.println("ERROR: " + errline);
                firstOutputFuture.cancel(false);
            });
        firstOutputFuture.get(10, TimeUnit.SECONDS);
        builder.checkpointViaJcmd();
        checkpointProcess.waitForCheckpointed();

        builder.startRestore().waitForSuccess()
            .outputAnalyzer().shouldContain(AFTER_RESTORE_MESSAGE);
    }

    @Override
    public void exec() throws RestoreException, CheckpointException {
        CountDownLatch start = new CountDownLatch(1);
        CountDownLatch finish = new CountDownLatch(1);
        Thread workerThread = new Thread(() -> {
            System.out.println("worker thread start");
            start.countDown();
            try {
                finish.await();
            } catch (InterruptedException e) {
                throw new RuntimeException(e);
            }
            System.out.println("worker thread finish");
        });
        assertFalse(workerThread.isDaemon());
        workerThread.start();

        try {
            start.await();
        } catch (InterruptedException e) {
            throw new RuntimeException(e);
        }

        Resource resource = new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
                assert Thread.currentThread().isDaemon() : "beforeCheckpoint is expected to be called from daemon thread";
                finish.countDown();
            }
            @Override
            public void afterRestore(Context<? extends Resource> context) throws Exception {
                Thread.sleep(3000);
                System.out.println(AFTER_RESTORE_MESSAGE);
            }
        };

        Core.getGlobalContext().register(resource);

        System.out.println(MAIN_THREAD_FINISH);
    }
}
