package jdk.internal.crac;

import jdk.crac.impl.CheckpointOpenResourceException;

public abstract class JDKResourceImpl implements JDKResource {
    final Exception stackTraceHolder;

    static volatile boolean hintPrinted = false;

    public JDKResourceImpl() {
        stackTraceHolder = JDKContext.Properties.COLLECT_FD_STACKTRACES ?
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
            LoggerContainer.info(JDKContext.COLLECT_FD_STACKTRACES_HINT);
        }
        return stackTraceHolder;
    }
}
