// Copyright 2019-2020 Azul Systems, Inc.  All Rights Reserved.
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


import java.nio.channels.Selector;
import java.nio.channels.ClosedSelectorException;
import java.io.IOException;
import java.util.Random;

import java.util.concurrent.atomic.AtomicInteger;


public class Test {

    private final static Random RND = new Random();

    private static void test(boolean skipCR, boolean closeBeforeCheckpoint) throws Exception {

        int nThreads = 20;

        AtomicInteger nSelected = new AtomicInteger(0);

        Selector selector = Selector.open();

        Thread selectThreads[] = new Thread[nThreads];

        Runnable rStart = new Runnable() {
            @Override
            public void run() {

                for (int i = 0; i < nThreads; ++i) {

                    selectThreads[i] = new Thread(new Runnable() {
                        @Override
                        public void run() {
                            try {
                                boolean timeout = RND.nextBoolean();
                                int n = nSelected.incrementAndGet();
                                System.out.println(">> select" + (timeout ? " (t)" : "") + " " + n);
                                if (timeout) { selector.select(10 + RND.nextInt(7_000)); }
                                else { selector.select(); }
                                nSelected.decrementAndGet();
                                System.out.println(">> done" + (timeout ? " (t)" : "") + " " + n);
                            } catch (ClosedSelectorException e) {
                                System.out.println(">> ClosedSelectorException"); // expected when the selector is closed
                                nSelected.decrementAndGet();
                            } catch (IOException e) {
                                throw new RuntimeException(e);
                            }
                        }
                    });
                    selectThreads[i].start();
                    try { Thread.sleep(50); } catch (InterruptedException ie) {}
                }
            }
        };
        Thread tStart = new Thread(rStart);
        tStart.start();
        Thread.sleep(500);

        if (closeBeforeCheckpoint) {
            tStart.join();
            Thread.sleep(1000);
            selector.close();
        }

        if (!skipCR) {
            jdk.crac.Core.checkpointRestore();
        }

        if (!closeBeforeCheckpoint) {
            tStart.join();
            Thread.sleep(1000);
            selector.close();
        }

        do { Thread.sleep(2000); } while (nSelected.get() > 0);

        if (nSelected.get() < 0) { throw new RuntimeException("negative nSelected??"); }

        // just in case...
        for (Thread t: selectThreads) { t.join(); }
    }



    public static void main(String args[]) throws Exception {

        if (args.length < 1) { throw new RuntimeException("test number is missing"); }

        switch (args[0]) {
            case "1":
                test(false, false);
                break;
            case "2":
                test(false, true);
                break;
            case "3":
                test(true, true);
                break;
            default:
                throw new RuntimeException("invalid test number");
        }
    }
}
