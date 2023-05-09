package jdk.internal.crac;

import jdk.crac.impl.CheckpointOpenResourceException;

public abstract class JDKResourceImpl implements JDKResource {
    final Exception stackTraceHolder;

    static volatile boolean hintPrinted = false;

    public JDKResourceImpl() {
        stackTraceHolder = JDKContext.Properties.COLLECT_FD_STACKTRACES ?
            new Exception("Resource Stack Trace") :
            null;

        Core.getJDKContext().register(this);
    }

    protected Exception getStackTraceHolder() {
        if (!hintPrinted && stackTraceHolder == null) {
            hintPrinted = true;
            LoggerContainer.info(JDKContext.COLLECT_FD_STACKTRACES_HINT);
        }
        return stackTraceHolder;
    }
}
