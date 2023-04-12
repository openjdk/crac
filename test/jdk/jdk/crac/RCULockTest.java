/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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

import jdk.crac.RCULock;

import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import static jdk.test.lib.Asserts.*;

/**
 * @test
 * @library /test/lib
 * @run main/timeout=20 RCULockTest
 */
public class RCULockTest {
    public static final int NUM_READERS = 50;
    private final RCULock lock = RCULock.forClasses(RCULockTest.class);
    private final AtomicBoolean stop = new AtomicBoolean();
    private final AtomicInteger readersIn = new AtomicInteger();
    private final AtomicReference<Throwable> exception = new AtomicReference<>();
    private final CountDownLatch exLatch = new CountDownLatch(1);
    private final CyclicBarrier completion = new CyclicBarrier(2 + NUM_READERS);

    public static void main(String[] args) throws Exception {
        new RCULockTest().run();
    }

    private void run() throws Exception {
        // just add some other methods to test that code path
        lock.amendCriticalMethods("foo.Bar(I)I");
        new Thread(this::synchronizer, "synchronizer").start();
        for (int i = 0; i < NUM_READERS; ++i) {
            new Thread(this::reader, "reader-" + i).start();
        }
        exLatch.await(10, TimeUnit.SECONDS);
        Throwable ex = exception.get();
        if (ex != null) {
            throw new AssertionError(ex);
        }
        stop.set(true);
        completion.await(10, TimeUnit.SECONDS);
    }

    private void reader() {
        try {
            while (!stop.get()) {
                try {
                    readerCritical();
                } finally {
                    lock.readUnlock();
                }
                Thread.sleep(1);
            }
            completion.await();
        } catch (Throwable t) {
            t.printStackTrace();
            exception.set(t);
            exLatch.countDown();
        }
    }

    @RCULock.Critical
    private void readerCritical() throws InterruptedException {
        lock.readLock();
        readersIn.incrementAndGet();
        Thread.sleep(ThreadLocalRandom.current().nextInt(10));
        readersIn.decrementAndGet();
    }

    private void synchronizer() {
        try {
            while (!stop.get()) {
                lock.synchronizeBegin();
                try {
                    assertEquals(0, readersIn.get());
                    Thread.sleep(ThreadLocalRandom.current().nextInt(10));
                    assertEquals(0, readersIn.get());
                } finally {
                    lock.synchronizeEnd();
                }
                Thread.sleep(ThreadLocalRandom.current().nextInt(10));
            }
            completion.await();
        } catch (Throwable t) {
            t.printStackTrace();
            exception.set(t);
            exLatch.countDown();
        }
    }
}
