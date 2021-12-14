/*
 * Copyright (c) 2017, 2021, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

package jdk.crac;

import jdk.crac.impl.CheckpointOpenFileException;
import jdk.crac.impl.CheckpointOpenResourceException;
import jdk.crac.impl.CheckpointOpenSocketException;
import jdk.crac.impl.OrderedContext;
import jdk.internal.misc.VM;

import java.security.AccessController;
import java.security.PrivilegedAction;
import java.util.ArrayList;
import java.util.List;

/**
 * The coordination service.
 */
public class Core {
    private static final int JVM_CHECKPOINT_OK    = 0;
    private static final int JVM_CHECKPOINT_ERROR = 1;
    private static final int JVM_CHECKPOINT_NONE  = 2;

    private static final int JVM_CR_FAIL = 0;
    private static final int JVM_CR_FAIL_FILE = 1;
    private static final int JVM_CR_FAIL_SOCK = 2;
    private static final int JVM_CR_FAIL_PIPE = 3;

    private static native Object[] checkpointRestore0();

    private static boolean traceStartupTime;

    private static final List<Object> lockQ = new ArrayList<Object>();
    private static final Object checkpointRestoreLock = new Object();
    private static final boolean isCheckpointConfigured = VM.isCheckpointConfigured();

    private static final Context<Resource> globalContext = new OrderedContext();
    static {
        // force JDK context initialization
        jdk.internal.crac.Core.getJDKContext();

        @SuppressWarnings("removal")
        boolean doTraceStartupTime = AccessController.doPrivileged(
                new PrivilegedAction<Boolean>() {
                    public Boolean run() {
                        return Boolean.parseBoolean(
                                System.getProperty("jdk.crac.trace-startup-time"));
                    }});

        traceStartupTime = doTraceStartupTime;
    }

    /** This class is not instantiable. */
    private Core() {
    }

    private static void translateJVMExceptions(int[] codes, String[] messages,
                                               CheckpointException newException) {
        assert codes.length == messages.length;
        final int length = codes.length;

        for (int i = 0; i < length; ++i) {
            switch(codes[i]) {
                case JVM_CR_FAIL_FILE:
                    newException.addSuppressed(
                            new CheckpointOpenFileException(messages[i]));
                    break;
                case JVM_CR_FAIL_SOCK:
                    newException.addSuppressed(
                            new CheckpointOpenSocketException(messages[i]));
                    break;
                case JVM_CR_FAIL_PIPE:
                    // FALLTHROUGH
                default:
                    newException.addSuppressed(
                            new CheckpointOpenResourceException(messages[i]));
                    break;
            }
        }
    }

    /**
     * Gets the global {@code Context} for checkpoint/restore notifications.
     *
     * @return the global {@code Context}
     */
    public static Context<Resource> getGlobalContext() {
        return globalContext;
    }

    private static void checkpointRestore1() throws
            CheckpointException,
            RestoreException {

        try {
            synchronized (lockQ) {
                // waiting to complete all already opened critical sections
                while (!lockQ.isEmpty()) {
                    try {
                        lockQ.wait();
                    } catch (InterruptedException iex) {
                        // just skip
                    }
                }
            }
            globalContext.beforeCheckpoint(null);
        } catch (CheckpointException ce) {
            // TODO make dry-run
            try {
                globalContext.afterRestore(null);
            } catch (RestoreException re) {
                CheckpointException newException = new CheckpointException();
                for (Throwable t : ce.getSuppressed()) {
                    newException.addSuppressed(t);
                }
                for (Throwable t : re.getSuppressed()) {
                    newException.addSuppressed(t);
                }
                throw newException;
            }
            throw ce;
        }
        final Object[] bundle = checkpointRestore0();
        final int retCode = (Integer) bundle[0];
        final int[] codes = (int[]) bundle[1];
        final String[] messages = (String[]) bundle[2];

        if (traceStartupTime) {
            System.out.println("STARTUPTIME " + System.nanoTime() + " restore");
        }

        if (retCode != JVM_CHECKPOINT_OK) {
            CheckpointException newException = new CheckpointException();
            switch (retCode) {
                case JVM_CHECKPOINT_ERROR:
                    translateJVMExceptions(codes, messages, newException);
                    break;
                case JVM_CHECKPOINT_NONE:
                    newException.addSuppressed(
                            new RuntimeException("C/R is not configured"));
                    break;
                default:
                    newException.addSuppressed(
                            new RuntimeException("Unknown C/R result: " + retCode));
            }

            try {
                globalContext.afterRestore(null);
            } catch (RestoreException re) {
                for (Throwable t : re.getSuppressed()) {
                    newException.addSuppressed(t);
                }
            }
            throw newException;
        }

        globalContext.afterRestore(null);
    }

    /**
     * Requests checkpoint and returns upon a successful restore.
     * May throw an exception if the checkpoint or restore are unsuccessful.
     *
     * @throws CheckpointException if an exception occured during checkpoint
     * notification and the execution continues in the original Java instance.
     * @throws RestoreException if an exception occured during restore
     * notification and execution continues in a new Java instance.
     * @throws UnsupportedOperationException if checkpoint/restore is not
     * supported, no notification performed and the execution continues in
     * the original Java instance.
     */
    public static void checkpointRestore() throws
            CheckpointException,
            RestoreException {
        synchronized (checkpointRestoreLock) {
            try {
                checkpointRestore1();
            } finally {
                if (traceStartupTime) {
                    System.out.println("STARTUPTIME " + System.nanoTime() + " restore-finish");
                }
            }
        }
    }

    /* called by VM */
    private static void checkpointRestoreInternal() {
        Thread thread = new Thread(() -> {
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
            }

            try {
                checkpointRestore();
            } catch (CheckpointException | RestoreException e) {
                for (Throwable t : e.getSuppressed()) {
                    t.printStackTrace();
                }
            }
        });
        thread.setDaemon(true);
        thread.start();
    }

    public static void criticalSection(Runnable action) {
        if (isCheckpointConfigured) {
            Object lock = new Object();
            addLock(lock);
            try {
                action.run();
            } finally {
                removeLock(lock);
            }
        } else {
            action.run();
        }
    }

    private static void addLock(Object lock) {
        synchronized(checkpointRestoreLock) {
            synchronized (lockQ) {
                lockQ.add(lock);
            }
        }
    }

    private static void removeLock(Object lock) {
        synchronized (lockQ) {
            lockQ.remove(lock);
            if(lockQ.isEmpty())
                lockQ.notify();
        }
    }
}
