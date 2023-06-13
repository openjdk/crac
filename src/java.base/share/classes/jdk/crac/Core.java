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

import jdk.crac.impl.*;
import jdk.internal.crac.JDKContext;
import jdk.internal.crac.JDKResource;
import jdk.internal.crac.LoggerContainer;

import sun.security.action.GetBooleanAction;
import sun.security.action.GetPropertyAction;

import java.io.PrintWriter;
import java.io.StringWriter;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.security.AccessController;
import java.security.PrivilegedAction;
import java.security.PrivilegedActionException;
import java.security.PrivilegedExceptionAction;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;
import java.util.function.Function;

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
    private static native Object[] checkpointRestore0(int[] fdArr, Object[] objArr, boolean dryRun, long jcmdStream);
    private static final Object checkpointRestoreLock = new Object();
    private static boolean checkpointInProgress = false;

    private static class FlagsHolder {
        private FlagsHolder() {}
        public static final boolean TRACE_STARTUP_TIME =
            GetBooleanAction.privilegedGetProperty("jdk.crac.trace-startup-time");
    }

    private static final Context<Resource> globalContext = new BlockingOrderedContext<>();

    /** This class is not instantiable. */
    private Core() {
    }

    private static void translateJVMExceptions(int[] codes, String[] messages,
                                               CheckpointException exception) {
        assert codes.length == messages.length;
        // When the CR engine fails (e.g. due to permissions, missing binaries...)
        // there are no messages recorded, but the user would end up with an empty
        // CheckpointException without stack trace nor message.
        if (codes.length == 0) {
            exception.addSuppressed(new RuntimeException("Native checkpoint failed."));
        }

        for (int i = 0; i < codes.length; ++i) {
            exception.addSuppressed(switch (codes[i]) {
                case JVM_CR_FAIL_FILE -> new CheckpointOpenFileException(messages[i], null);
                case JVM_CR_FAIL_SOCK -> new CheckpointOpenSocketException(messages[i]);
                case JVM_CR_FAIL_PIPE -> new CheckpointOpenResourceException(messages[i], null);
                default -> new CheckpointOpenResourceException(messages[i], null);
            });
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

    @SuppressWarnings("removal")
    private static void checkpointRestore1(long jcmdStream) throws
            CheckpointException,
            RestoreException {
        CheckpointException checkpointException = null;
        // This log is here to initialize call sites in logger formatters.
        LoggerContainer.debug("Starting checkpoint at epoch:{0}", System.currentTimeMillis());

        try {
            jdk.internal.crac.Core.getJDKContext().beforeCheckpoint(null);
            globalContext.beforeCheckpoint(null);
        } catch (CheckpointException ce) {
            checkpointException = ce;
        }

        JDKContext jdkContext = jdk.internal.crac.Core.getJDKContext();
        List<Map.Entry<Integer, Object>> claimedPairs = jdkContext.getClaimedFds().entrySet().stream().toList();
        int[] fdArr = new int[claimedPairs.size()];
        Object[] objArr = new Object[claimedPairs.size()];
        LoggerContainer.debug("Claimed open file descriptors:");
        for (int i = 0; i < claimedPairs.size(); ++i) {
            fdArr[i] = claimedPairs.get(i).getKey();
            objArr[i] = claimedPairs.get(i).getValue();
            LoggerContainer.debug( "\t{0} {1}", fdArr[i], objArr[i]);
        }

        final Object[] bundle = checkpointRestore0(fdArr, objArr, checkpointException != null, jcmdStream);
        final int retCode = (Integer)bundle[0];
        final String newArguments = (String)bundle[1];
        final String[] newProperties = (String[])bundle[2];
        final int[] codes = (int[])bundle[3];
        final String[] messages = (String[])bundle[4];

        if (FlagsHolder.TRACE_STARTUP_TIME) {
            System.out.println("STARTUPTIME " + System.nanoTime() + " restore");
        }

        if (retCode != JVM_CHECKPOINT_OK) {
            if (checkpointException == null) {
                checkpointException = new CheckpointException();
            }
            switch (retCode) {
                case JVM_CHECKPOINT_ERROR -> translateJVMExceptions(codes, messages, checkpointException);
                case JVM_CHECKPOINT_NONE -> checkpointException.addSuppressed(new RuntimeException("C/R is not configured"));
                default -> checkpointException.addSuppressed(new RuntimeException("Unknown C/R result: " + retCode));
            }
        }

        if (newProperties != null && newProperties.length > 0) {
            // Do not use lambda here since lambda will introduce registration
            // during checkpoint, which may cause dead loop.
            Arrays.stream(newProperties).map(new Function<String, String[]>() {
                @Override
                public String[] apply(String propStr) {
                    return propStr.split("=", 2);
                }
            }).forEach(new Consumer<String[]>() {
                @Override
                public void accept(String[] pair) {
                    AccessController.doPrivileged(
                            new PrivilegedAction<String>() {
                                @Override
                                public String run() {
                                    return System.setProperty(pair[0], pair.length == 2 ? pair[1] : "");
                                }
                            });
                }
            });
        }

        RestoreException restoreException = null;
        try {
            globalContext.afterRestore(null);
            jdk.internal.crac.Core.getJDKContext().afterRestore(null);
        } catch (RestoreException re) {
            if (checkpointException == null) {
                restoreException = re;
            } else {
                for (Throwable t : re.getSuppressed()) {
                    checkpointException.addSuppressed(t);
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
                    assert checkpointException == null :
                        "should not have new arguments";
                    if (restoreException == null) {
                        restoreException = new RestoreException();
                    }
                    restoreException.addSuppressed(e);
                }
            }
        }

        assert checkpointException == null || restoreException == null;
        if (checkpointException != null) {
            throw checkpointException;
        } else if (restoreException != null) {
            throw restoreException;
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

    @SuppressWarnings("try")
    private static void checkpointRestore(long jcmdStream) throws
            CheckpointException,
            RestoreException {
        // checkpointRestoreLock protects against the simultaneous
        // call of checkpointRestore from different threads.
        synchronized (checkpointRestoreLock) {
            // checkpointInProgress protects against recursive
            // checkpointRestore from resource's
            // beforeCheckpoint/afterRestore methods
            if (checkpointInProgress) {
                CheckpointException ex = new CheckpointException();
                ex.addSuppressed(new IllegalStateException("Recursive checkpoint is not allowed"));
                throw ex;
            }

            try (@SuppressWarnings("unused") var keepAlive = new KeepAlive()) {
                checkpointInProgress = true;
                checkpointRestore1(jcmdStream);
            } finally {
                if (FlagsHolder.TRACE_STARTUP_TIME) {
                    System.out.println("STARTUPTIME " + System.nanoTime() + " restore-finish");
                }
                checkpointInProgress = false;
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
