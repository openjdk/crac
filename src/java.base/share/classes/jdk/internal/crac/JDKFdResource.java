package jdk.internal.crac;

import sun.security.action.GetBooleanAction;

public abstract class JDKFdResource implements JDKResource {
    private static final String COLLECT_FD_STACKTRACES_PROPERTY = "jdk.crac.collect-fd-stacktraces";
    private static final String COLLECT_FD_STACKTRACES_HINT =
        "Use -D" + COLLECT_FD_STACKTRACES_PROPERTY + "=true to find the source.";

    private static final boolean COLLECT_FD_STACKTRACES =
        GetBooleanAction.privilegedGetProperty(COLLECT_FD_STACKTRACES_PROPERTY);

    final Exception stackTraceHolder;

    static volatile boolean hintPrinted = false;

    public JDKFdResource() {
        stackTraceHolder = COLLECT_FD_STACKTRACES ?
            // About the timestamp: we cannot format it nicely since this
            // exception is sometimes created too early in the VM lifecycle
            // (but it's hard to detect when it would be safe to do).
            new Exception("This file descriptor was created by " + Thread.currentThread().getName()
                + " at epoch:" + System.currentTimeMillis() + " here") :
            null;

        Core.Priority.FILE_DESCRIPTORS.getContext().register(this);
    }

    protected Exception getStackTraceHolder() {
        if (!hintPrinted && stackTraceHolder == null) {
            hintPrinted = true;
            LoggerContainer.info(COLLECT_FD_STACKTRACES_HINT);
        }
        return stackTraceHolder;
    }
}
