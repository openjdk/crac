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
import java.io.StringWriter;
import java.io.PrintWriter;
import sun.security.action.GetBooleanAction;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.security.AccessController;
import java.security.PrivilegedAction;
import java.security.PrivilegedActionException;
import java.security.PrivilegedExceptionAction;
import java.util.ArrayList;
import java.util.Arrays;
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

    private static final long JCMD_STREAM_NULL = 0;

    private static native Object[] checkpointRestore0(boolean dryRun, long jcmdStream);
    private static final Object checkpointRestoreLock = new Object();
    private static boolean checkpointInProgress = false;
    private static boolean restoring = false;
    private static List<Throwable> checkpointExceptions;

    private static class FlagsHolder {
        public static final boolean TRACE_STARTUP_TIME =
            GetBooleanAction.privilegedGetProperty("jdk.crac.trace-startup-time");
    }

    private static final Context<Resource> globalContext = new OrderedContext<>("GlobalContext");
    static {
        // force JDK context initialization
        jdk.internal.crac.Core.getJDKContext();
    }

    /** This class is not instantiable. */
    private Core() {
    }

    private static void translateJVMExceptions(int[] codes, String[] messages) {
        assert codes.length == messages.length;
        final int length = codes.length;

        for (int i = 0; i < length; ++i) {
            Throwable ex = switch (codes[i]) {
                case JVM_CR_FAIL_FILE -> new CheckpointOpenFileException(messages[i]);
                case JVM_CR_FAIL_SOCK -> new CheckpointOpenSocketException(messages[i]);
                case JVM_CR_FAIL_PIPE -> new CheckpointOpenResourceException(messages[i]);
                default -> new CheckpointOpenResourceException(messages[i]);
            };
            recordException(ex);
        }
    }

    /**
     * Gets the global {@code Context} for checkpoint/restore notifications
     * with the following properties:
     * <ul>
     * <li>The context maintains a weak reference to registered {@link Resource}.
     *     Therefore, it is important for the registrar to keep another strong
     *     reference to the resource - otherwise the garbage collector
     *     is free to trash the resource and notifications on this resource
     *     will not be invoked.
     * <li>Order of invoking {@link Resource#beforeCheckpoint(Context)} is
     *     the reverse of the order of {@linkplain Context#register(Resource)
     *     registration}.
     * <li>Order of invoking {@link Resource#afterRestore(Context)} is
     *     the reverse of the order of {@linkplain Resource#beforeCheckpoint(Context)
     *     checkpoint notification}, hence the same as the order of
     *     {@link Context#register(Resource) registration}.
     * <li>{@code Resource} is always notified of checkpoint or restore,
     *     regardless of whether other {@code Resource} notifications have
     *     thrown an exception or not,
     * <li>When an exception is thrown during notification it is caught by
     *     the {@code Context} and is suppressed by a {@link CheckpointException}
     *     or {@link RestoreException}, depends on the throwing method.
     * </ul>
     *
     * @return the global {@code Context}
     */
    public static Context<Resource> getGlobalContext() {
        return globalContext;
    }

    public static void recordException(Throwable e) {
        checkpointExceptions.add(e);
    }

    /**
     * Checks if we are currently invoking {@link Resource#afterRestore(Context)}
     * notifications (whether this is after a successful checkpoint or
     * compensating for a failed one). Calling this from a different thread
     * than the one performing the restore is subject to races.
     *
     * @return True if invoking <code>afterRestore</code>, false if the C/R
     * is not in progress, or it is yet in the checkpoint phase.
     */
    public static synchronized boolean isRestoring() {
        return restoring;
    }

    @SuppressWarnings("removal")
    private static void checkpointRestore1(long jcmdStream) throws
            CheckpointException,
            RestoreException {

        try {
            globalContext.beforeCheckpoint(null);
        } catch (CheckpointException ce) {
            for (Throwable t : ce.getSuppressed()) {
                recordException(t);
            }
        }

        boolean checkpointHasExceptions = !checkpointExceptions.isEmpty();
        restoring = true;

        final Object[] bundle = checkpointRestore0(checkpointHasExceptions, jcmdStream);
        final int retCode = (Integer)bundle[0];
        final String newArguments = (String)bundle[1];
        final String[] newProperties = (String[])bundle[2];
        final int[] codes = (int[])bundle[3];
        final String[] messages = (String[])bundle[4];

        if (FlagsHolder.TRACE_STARTUP_TIME) {
            System.out.println("STARTUPTIME " + System.nanoTime() + " restore");
        }

        if (retCode != JVM_CHECKPOINT_OK) {
            switch (retCode) {
                case JVM_CHECKPOINT_ERROR -> translateJVMExceptions(codes, messages);
                case JVM_CHECKPOINT_NONE -> recordException(new RuntimeException("C/R is not configured"));
                default -> recordException(new RuntimeException("Unknown C/R result: " + retCode));
            }
        }

        if (newProperties != null && newProperties.length > 0) {
            Arrays.stream(newProperties).map(propStr -> propStr.split("=", 2)).forEach(pair -> {
                AccessController.doPrivileged(
                    (PrivilegedAction<String>)() ->
                        System.setProperty(pair[0], pair.length == 2 ? pair[1] : ""));
            });
        }

        try {
            globalContext.afterRestore(null);
        } catch (RestoreException re) {
            if (re.getSuppressed().length == 0) {
                recordException(re);
            } else {
                for (Throwable t : re.getSuppressed()) {
                    recordException(t);
                }
            }
        }

        if (newArguments != null && newArguments.length() > 0) {
            String[] args = newArguments.split(" ");
            if (args.length > 0) {
                try {
                    Method newMain = AccessController.doPrivileged(new PrivilegedExceptionAction<Method>() {
                       @Override
                       public Method run() throws Exception {
                           Class < ?> newMainClass = Class.forName(args[0], false,
                               ClassLoader.getSystemClassLoader());
                           Method newMain = newMainClass.getDeclaredMethod("main",
                               String[].class);
                           newMain.setAccessible(true);
                           return newMain;
                       }
                    });
                    newMain.invoke(null,
                        (Object)Arrays.copyOfRange(args, 1, args.length));
                } catch (PrivilegedActionException |
                         InvocationTargetException |
                         IllegalAccessException e) {
                    recordException(e);
                }
            }
        }

        if (!checkpointExceptions.isEmpty()) {
            if (checkpointHasExceptions) {
                CheckpointException ce = new CheckpointException();
                for (Throwable checkpointException : checkpointExceptions) {
                    ce.addSuppressed(checkpointException);
                }
                throw ce;
            } else {
                RestoreException re = new RestoreException();
                for (Throwable checkpointException : checkpointExceptions) {
                    re.addSuppressed(checkpointException);
                }
                throw re;
            }
        }
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
        checkpointRestore(JCMD_STREAM_NULL);
    }

    private static void checkpointRestore(long jcmdStream) throws
            CheckpointException,
            RestoreException {
        // checkpointRestoreLock protects against the simultaneous
        // call of checkpointRestore from different threads.
        synchronized (checkpointRestoreLock) {
            // checkpointInProgress protects against recursive
            // checkpointRestore from resource's
            // beforeCheckpoint/afterRestore methods
            if (!checkpointInProgress) {
                checkpointInProgress = true;
                assert !restoring;
                checkpointExceptions = new ArrayList<>();
                try {
                    checkpointRestore1(jcmdStream);
                } finally {
                    if (FlagsHolder.TRACE_STARTUP_TIME) {
                        System.out.println("STARTUPTIME " + System.nanoTime() + " restore-finish");
                    }
                    checkpointInProgress = false;
                    restoring = false;
                }
            } else {
                throw new CheckpointException("Recursive checkpoint is not allowed");
            }
        }
    }

    /* called by VM */
    private static String checkpointRestoreInternal(long jcmdStream) {
        try {
            checkpointRestore(jcmdStream);
        } catch (CheckpointException e) {
            StringWriter sw = new StringWriter();
            PrintWriter pw = new PrintWriter(sw);
            e.printStackTrace(pw);
            return sw.toString();
        } catch (RestoreException e) {
            e.printStackTrace();
            return null;
        }
        return null;
    }
}
