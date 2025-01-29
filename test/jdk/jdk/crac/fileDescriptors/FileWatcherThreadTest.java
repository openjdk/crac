/*
 * Copyright (c) 2024, Azul Systems, Inc. All rights reserved.
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
import jdk.test.lib.Asserts;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.File;
import java.io.IOException;
import java.nio.file.*;
import java.util.Objects;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.BrokenBarrierException;
import java.util.concurrent.CyclicBarrier;

/**
 * @test Checks the different state of WatchService keys at a checkpoint moment.
 * @library /test/lib
 * @build FileWatcherThreadTest
 * @run driver jdk.test.lib.crac.CracTest true
 * @run driver jdk.test.lib.crac.CracTest false
 * @requires (os.family == "linux")
 */
public class FileWatcherThreadTest implements CracTest {

    @CracTestArg(0)
    boolean checkpointWithoutKey;

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().verbose(true);
        builder.doCheckpointAndRestore();
    }

    @Override
    public void exec() throws Exception {
        Path directory = Paths.get(System.getProperty("user.dir"), "workdir");
        directory.toFile().mkdir();

        CompletableFuture<Boolean> caughtFirst = new CompletableFuture();
        CompletableFuture<Boolean> caughtSecond = new CompletableFuture();
        CyclicBarrier barrier = new CyclicBarrier(2); // Create a CyclicBarrier to sync between two threads

        // Start the WatchService in a separate thread
        Thread watchThread = new Thread(() -> {
            try {
                WatchService watchService = FileSystems.getDefault().newWatchService();
                directory.register(watchService, StandardWatchEventKinds.ENTRY_CREATE);

                if (checkpointWithoutKey) {
                    barrier.await(); // Wait until the barrier is ready
                }

                while (true) {
                    WatchKey key = watchService.take(); // Blocking call, waits for an event
                    if (key == null) continue;

                    for (WatchEvent<?> event : key.pollEvents()) {
                        if (event.kind() == StandardWatchEventKinds.ENTRY_CREATE) {
                            if (caughtFirst.isDone()) {
                                caughtSecond.complete(true);
                            } else {
                                caughtFirst.complete(true);
                            }
                        }
                    }
                    key.reset();
                }
            } catch (Throwable e) {
                if (caughtFirst.isDone()) {
                    caughtSecond.completeExceptionally(e);
                } else {
                    caughtFirst.completeExceptionally(e);
                }
            }
        });

        watchThread.setDaemon(true);
        watchThread.start();

        if (checkpointWithoutKey) {
            barrier.await(); // Wait for the WatchService to be ready
            Core.checkpointRestore(); // Restore from checkpoint
            Files.createTempFile(directory, "temp", ".txt");
            Asserts.assertFalse(caughtSecond.isDone());
        } else {
            Files.createTempFile(directory, "temp", ".txt");
            Asserts.assertTrue(caughtFirst.get());
            Core.checkpointRestore();
            Files.createTempFile(directory, "temp", ".txt");
            Asserts.assertTrue(caughtSecond.get());
        }
    }
}
