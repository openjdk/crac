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
import java.security.SecureRandom;
import java.util.concurrent.atomic.AtomicLong;

public class Test implements Resource {

    private static final AtomicLong counter = new AtomicLong(0);
    private static boolean stop = false;
    private static final long MIN_TIMEOUT = 100;
    private static final long MAX_TIMEOUT = 1000;
    private static SecureRandom sr;

    private static class TestThread1 extends Thread {
        private long timeout;

        TestThread1(long timeout) {
            this.timeout = timeout;
        }

        @Override
        public void run() {
            while (!stop) {
                Test.set();
            }
        }
    };

    private static class TestThread2 extends Thread implements Resource {
        private long timeout;
        private SecureRandom sr;

        synchronized void set() {
            sr.nextInt();
        }
        synchronized void clean() {
            sr.nextInt();
        }

        TestThread2(long timeout) throws Exception {
            this.timeout = timeout;
            sr = SecureRandom.getInstance("SHA1PRNG");
            Core.getGlobalContext().register(this);
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
    };

    synchronized static void clean() {
        sr.nextInt();
    }

    synchronized static void set() {
        sr.nextInt();
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        try {
            clean();
        } catch(Exception e) {
            e.printStackTrace(System.out);
        };
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        set();
        stop = true;
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
        test.sr = SecureRandom.getInstance("SHA1PRNG");
        Core.getGlobalContext().register(test);

        Random random = new Random();
        Thread[] threads = new Thread[numThreads];
        for(int i=0; i<numThreads; i++) {
            threads[i] = (i%2==0)?
                    new TestThread1(random.nextLong(MAX_TIMEOUT - MIN_TIMEOUT) + MIN_TIMEOUT):
                    new TestThread2(random.nextLong(MAX_TIMEOUT - MIN_TIMEOUT) + MIN_TIMEOUT);
            threads[i].start();
        };
        Thread.currentThread().sleep(MIN_TIMEOUT);
        set();
        Thread.currentThread().sleep(MIN_TIMEOUT);

        Object checkpointLock = new Object();
        Thread checkpointThread = new Thread("checkpointThread") {
            public void run() {
                synchronized (checkpointLock) {
                    try {
                        jdk.crac.Core.checkpointRestore();
                    } catch (CheckpointException e) {
                        throw new RuntimeException("Checkpoint ERROR " + e);
                    } catch (RestoreException e) {
                        throw new RuntimeException("Restore ERROR " + e);
                    }
                    checkpointLock.notify();
                }
            }
        };
        synchronized (checkpointLock) {
            try {
                checkpointThread.start();
                checkpointLock.wait(MAX_TIMEOUT * 2);
            } catch(Exception e){
                throw new RuntimeException("Checkpoint/Restore ERROR " + e);
            }
        }
        Thread.currentThread().sleep(MAX_TIMEOUT);
    }
}
