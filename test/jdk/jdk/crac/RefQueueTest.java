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

import java.io.*;
import java.lang.ref.Cleaner;
import java.lang.ref.Reference;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.WeakReference;

import jdk.crac.*;

/**
 * @test
 * @run main/othervm -XX:CREngine=simengine -XX:CRaCCheckpointTo=./cr RefQueueTest
 */
public class RefQueueTest {
    private static final Cleaner cleaner = Cleaner.create();

    static class Tuple {
        private Object object = new Object();
        private ReferenceQueue<WeakReference> queue = new ReferenceQueue<>();
        private Reference ref = new WeakReference(object, queue);
        private Thread thread;

        Tuple(Runnable r) {
            thread = new Thread(() -> {
                while (true) {
                    try {
                        queue.remove();
                        if (r != null) {
                            r.run();
                        }
                    } catch (InterruptedException e) {
                        e.printStackTrace();
                    }
                }
            });
            thread.setDaemon(true);
            thread.start();
        }

        Object getObject() {
            return object;
        }

        void clearObject() {
            object = null;
        }

        void waitProcessed() throws InterruptedException {
            Misc.waitForQueueProcessed(queue, 1, 0);
        }
    }

    static public void main(String[] args) throws Exception {

        File badFile = File.createTempFile("jtreg-RefQueueTest", null);
        OutputStream badStream = new FileOutputStream(badFile);
        badStream.write('j');
        badFile.delete();

        Tuple[] tuples = new Tuple[10];
        for (int i = 0; i < tuples.length - 1; ++i) {
            int ii = i;
            tuples[i] = new Tuple(() -> {
                System.out.println("WOKE " + ii);
                tuples[ii + 1].clearObject();
            });
        }
        tuples[tuples.length - 1] = new Tuple(() -> {
            System.out.println("WOKE " + (tuples.length - 1));
        });

        // the cleaner should run only after user reference processing complete
        cleaner.register(tuples[tuples.length - 1].getObject(), () -> {
            System.out.println("CLEANER");
            try {
                badStream.close();
            } catch (IOException e) {
                throw new RuntimeException(e);
            }
        });

        Resource testResource = new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context)
                throws Exception
            {
                tuples[0].clearObject();

                // should return quickly: no references yet. But the
                // call is valid.
                System.out.println("ATTEMPT 1");
                tuples[tuples.length - 1].waitProcessed();

                // Now make sure that all necessary processing has happened.
                // We do this in a way that is specific to this app.
                System.out.println("ATTEMPT " + tuples.length);
                for (int i = 0; i < tuples.length; ++i) {
                    tuples[i].waitProcessed();
                }
                System.out.println("ATTEMPT done");
            }

            @Override
            public void afterRestore(Context<? extends Resource> context) throws Exception {

            }
        };
        jdk.crac.Core.getGlobalContext().register(testResource);

        // should close the file and only then go to the native checkpoint
        Core.checkpointRestore();
    }
}
