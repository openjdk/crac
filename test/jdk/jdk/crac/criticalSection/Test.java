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
import java.util.Random;
import java.util.concurrent.atomic.AtomicLong;


public class Test implements Resource {

    private static final AtomicLong counter = new AtomicLong(0);
    private static boolean stop = false;
    private static final long MIN_TIMEOUT = 100;
    private static final long MAX_TIMEOUT = 1000;

    private static class TestThread extends Thread {
        private long timeout;

        TestThread(long timeout) {
            this.timeout = timeout;
        }

        @Override
        public void run() {
            while (!stop) {
                Core.criticalSection( () -> {
                    try {
                        if (stop)
                            return;
                        counter.incrementAndGet();
                        Thread.sleep(timeout);
                        counter.decrementAndGet();
                    } catch (InterruptedException ie) {
                        throw new RuntimeException(ie);
                    }
                });
            }
        }
    };

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        long ccounter = counter.get();
        if (ccounter != 0)
            throw new RuntimeException("Incorrect counter before checkpoint: " + ccounter + " instead of 0");
        stop = true;
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
    }

    public static void main(String args[]) throws Exception {
        if (args.length < 1) { throw new RuntimeException("number of threads is missing"); }
        int numThreads;
        try{
            numThreads = Integer.parseInt(args[0]);
        } catch (NumberFormatException ex){
            throw new RuntimeException("invalid number of threads");
        }
        Test test = new Test();
        Core.getGlobalContext().register(test);

        Random random = new Random();
        TestThread[] threads = new TestThread[numThreads];
        for(int i=0; i<numThreads; i++) {
            threads[i] = new TestThread(random.nextLong(MAX_TIMEOUT - MIN_TIMEOUT) + MIN_TIMEOUT);
            threads[i].start();
        };

        Thread.currentThread().sleep(MIN_TIMEOUT);
        try {
            jdk.crac.Core.checkpointRestore();
        } catch (CheckpointException e) {
            throw new RuntimeException("Checkpoint ERROR " + e);
        } catch (RestoreException e) {
            throw new RuntimeException("Restore ERROR " + e);
        }
        Thread.currentThread().sleep(MAX_TIMEOUT);
        long ccounter = counter.get();
        if (ccounter != 0)
            throw new RuntimeException("Incorrect counter after restore: " + ccounter + " instead of 0");
    }
}
