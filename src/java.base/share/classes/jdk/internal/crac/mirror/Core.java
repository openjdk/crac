/*
 * Copyright (c) 2017, 2025, Azul Systems, Inc. All rights reserved.
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

package jdk.internal.crac.mirror;

import jdk.internal.access.SharedSecrets;
import jdk.internal.crac.ClaimedFDs;
import jdk.internal.crac.JDKResource;
import jdk.internal.crac.LoggerContainer;
import jdk.internal.crac.mirror.impl.*;
import jdk.internal.misc.InnocuousThread;

import java.io.PrintWriter;
import java.io.StringWriter;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.security.AccessController;
import java.security.PrivilegedActionException;
import java.security.PrivilegedExceptionAction;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Supplier;

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

    private static final String ENABLE_RECOMPILATION_PROPERTY = "jdk.crac.enable-recompilation";
    private static final String RECOMPILATION_DELAY_MS_PROPERTY = "jdk.crac.recompilation-delay-ms";
    private static class FlagsHolder {
        private FlagsHolder() {}
        public static final boolean TRACE_STARTUP_TIME =
            Boolean.getBoolean("jdk.crac.trace-startup-time");
        public static final boolean ENABLE_RECOMPILATION =
            Boolean.parseBoolean(System.getProperty(ENABLE_RECOMPILATION_PROPERTY, "true"));
    }

    private static final Context<Resource> globalContext = GlobalContext.createGlobalContextImpl();

    private static class ReferenceHandlerResource implements JDKResource {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            System.gc();
            // TODO ensure GC done processing all References
            while (SharedSecrets.getJavaLangRefAccess().waitForReferenceProcessing());
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) throws Exception {
        }

        private static ReferenceHandlerResource resource = new ReferenceHandlerResource();

        static {
            jdk.internal.crac.Core.Priority.REFERENCE_HANDLER.getContext().register(resource);
        }

        /**
         * Performs one-time registration of the Reference handling resource
         */
        public static void register() {
            // nothing to do: the resource registered in the static initializer
        }
    }

    /** This class is not instantiable. */
    private Core() {
    }

    private static void translateJVMExceptions(int[] codes, String[] messages,
                                               ExceptionHolder<CheckpointException> exception) {
        assert codes.length == messages.length;
        // When the CR engine fails (e.g. due to permissions, missing binaries...)
        // there are no messages recorded, but the user would end up with an empty
        // CheckpointException without stack trace nor message.
        if (codes.length == 0) {
            exception.handle(new RuntimeException("Native checkpoint failed."));
        }

        for (int i = 0; i < codes.length; ++i) {
            Exception ex = switch (codes[i]) {
                case JVM_CR_FAIL_FILE -> new CheckpointOpenFileException(messages[i], null);
                case JVM_CR_FAIL_SOCK -> new CheckpointOpenSocketException(messages[i], null);
                case JVM_CR_FAIL_PIPE -> new CheckpointOpenResourceException(messages[i], null);
                default -> new CheckpointOpenResourceException(messages[i], null);
            };
            exception.handle(ex);
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
        final ExceptionHolder<CheckpointException> checkpointException = new ExceptionHolder<>(CheckpointException::new);

        // Register the resource here late, to avoid early registration
        // during JDK initialization, e.g. if performed during j.l.r.Reference
        // initialization.
        ReferenceHandlerResource.register();

        // FIXME: log something to complete logger initialization:
        // - call sites in logger formatters.
        // - FileDescriptors for resources (sun.util.calendar.ZoneInfoFile)
        LoggerContainer.info("Starting checkpoint");
        LoggerContainer.debug("at epoch:{0}", System.currentTimeMillis());

        ClaimedFDs claimedFDs = new ClaimedFDs();

        jdk.internal.crac.Core.setClaimedFDs(claimedFDs);
        try {
            globalContext.beforeCheckpoint(null);
        } catch (CheckpointException ce) {
            checkpointException.handle(ce);
        }
        jdk.internal.crac.Core.setClaimedFDs(null);

        List<ClaimedFDs.Descriptor> claimedList = claimedFDs.getClaimedFds();
        int[] fdArr = new int[claimedList.size()];
        LoggerContainer.debug("Claimed open file descriptors:");
        for (int i = 0; i < claimedList.size(); ++i) {
            ClaimedFDs.Descriptor desc = claimedList.get(i);
            LoggerContainer.debug("\t{0} {1} {2}", desc.getFd(), desc.getClaimer(), desc.getExceptionSupplier());
            fdArr[i] = desc.getFd();

            Supplier<Exception> exceptionSupplier = desc.getExceptionSupplier();
            if (exceptionSupplier != null) {
                Exception e = exceptionSupplier.get();
                checkpointException.handle(e);
            }
        }

        final Object[] bundle = checkpointRestore0(fdArr, null, checkpointException.hasException(), jcmdStream);
        final int retCode = (null == bundle) ? JVM_CHECKPOINT_NONE : (Integer)bundle[0];
        final String newArguments = (null == bundle) ? null : (String)bundle[1];
        final String[] newProperties = (null == bundle) ? null : (String[])bundle[2];
        final int[] codes = (null == bundle) ? null : (int[])bundle[3];
        final String[] messages = (null == bundle) ? null : (String[])bundle[4];

        if (FlagsHolder.TRACE_STARTUP_TIME) {
            System.out.println("STARTUPTIME " + System.nanoTime() + " restore");
        }

        if (retCode != JVM_CHECKPOINT_OK) {
            switch (retCode) {
                case JVM_CHECKPOINT_ERROR -> translateJVMExceptions(codes, messages, checkpointException);
                case JVM_CHECKPOINT_NONE -> checkpointException.handle(new RuntimeException("C/R is not configured"));
                default ->                  checkpointException.handle(new RuntimeException("Unknown C/R result: " + retCode));
            }
        }

        if (newProperties != null) {
            for (var prop : newProperties) {
                String[] pair = prop.split("=", 2); // There should always be a "="
                System.setProperty(pair[0], pair[1]);
            }
        }

        ExceptionHolder<RestoreException> restoreException = new ExceptionHolder<>(RestoreException::new);
        try {
            globalContext.afterRestore(null);
        } catch (RestoreException re) {
            if (checkpointException.hasException()) {
                checkpointException.resuppress(re);
            } else {
                restoreException.handle(re);
            }
        }

        // The agent should be started only after all Resources are restored so that it finds
        // the system in consistent state and is free to register more Resources (e.g. because
        // it may open configuration file for JMX).
        if (newProperties != null && Arrays.stream(newProperties).anyMatch(prop -> prop.startsWith("com.sun.management")) ||
                System.getProperties().keySet().stream().anyMatch(prop -> prop.toString().startsWith("com.sun.management"))) {
            tryStartManagementAgent();
        }

        if (newArguments != null && newArguments.length() > 0) {
            // Parse arguments into array, unescape spaces
            final char escChar = '\\';
            final char sepChar = ' ';
            final var argList = new ArrayList<String>();
            final var curArgBuilder = new StringBuilder();
            for (int i = 0; i < newArguments.length(); ++i) {
                final char curChar = newArguments.charAt(i);
                switch (curChar) {
                    case sepChar -> {
                        argList.add(curArgBuilder.toString());
                        curArgBuilder.setLength(0);
                    }
                    case escChar -> curArgBuilder.append(newArguments.charAt(++i));
                    default -> curArgBuilder.append(curChar);
                }
            }
            argList.add(curArgBuilder.toString());

            final String[] args = argList.toArray(new String[argList.size()]);
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
                    assert !checkpointException.hasException() :
                        "should not have new arguments";
                    restoreException.handle(e);
                }
            }
        }

        assert !checkpointException.hasException() || !restoreException.hasException();
        checkpointException.throwIfAny();
        restoreException.throwIfAny();
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

            startRecordingDecompilations();
            try (@SuppressWarnings("unused") var keepAlive = new KeepAlive()) {
                checkpointInProgress = true;
                checkpointRestore1(jcmdStream);
            } finally {
                if (FlagsHolder.TRACE_STARTUP_TIME) {
                    System.out.println("STARTUPTIME " + System.nanoTime() + " restore-finish");
                }
                checkpointInProgress = false;
                scheduleFinishRecordingDecompilationsAndRecompile();
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

    private static void tryStartManagementAgent() {
        try {
            Class<?> agentClass = Class.forName("jdk.internal.agent.Agent");
            Method startAgent = agentClass.getMethod("startAgent");
            startAgent.setAccessible(true);
            startAgent.invoke(null);
        } catch (ClassNotFoundException | NoSuchMethodException e) {
            LoggerContainer.error(e, "Cannot find JMX agent start method");
        } catch (InvocationTargetException | IllegalAccessException e) {
            LoggerContainer.error(e, "Cannot start JMX agent");
        }
    }

    private static Thread recompilerThread;

    private static native void startRecordingDecompilations0();
    private static native void finishRecordingDecompilationsAndRecompile0();

    private static void startRecordingDecompilations() throws CheckpointException {
        if (!FlagsHolder.ENABLE_RECOMPILATION) {
            return;
        }

        if (recompilerThread != null) {
            // Finish the existing recording, if any
            recompilerThread.interrupt();
            try {
                recompilerThread.join();
            } catch (InterruptedException ie) {
                final CheckpointException ex = new CheckpointException();
                ex.addSuppressed(ie);
                throw ex;
            }
        }
        startRecordingDecompilations0();
    }

    private static void scheduleFinishRecordingDecompilationsAndRecompile() {
        if (!FlagsHolder.ENABLE_RECOMPILATION) {
            if (System.getProperty(RECOMPILATION_DELAY_MS_PROPERTY) != null) {
                System.err.printf("Ignoring '%s' because '%s' is false\n",
                        RECOMPILATION_DELAY_MS_PROPERTY, ENABLE_RECOMPILATION_PROPERTY);
            }
            return;
        }

        final var recompilationDelayMs = Long.getLong(RECOMPILATION_DELAY_MS_PROPERTY, 10L);
        if (recompilationDelayMs <= 0) {
            finishRecordingDecompilationsAndRecompile0();
        } else {
            // InnocuousThread not to add a thread into the user's thread group
            final Thread t = InnocuousThread.newThread("CRaC Recompiler", () -> {
                try {
                    Thread.sleep(recompilationDelayMs);
                } catch (InterruptedException ignored) {
                    // Just finish earlier
                } finally {
                    finishRecordingDecompilationsAndRecompile0();
                }
            });
            t.setDaemon(true);
            t.start();
            recompilerThread = t;
        }
    }
}
