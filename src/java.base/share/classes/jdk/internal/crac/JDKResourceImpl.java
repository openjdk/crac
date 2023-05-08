package jdk.internal.crac;

import jdk.crac.impl.CheckpointOpenResourceException;

public abstract class JDKResourceImpl implements JDKResource {
    final Exception stackTraceHolder;

    public JDKResourceImpl() {
        stackTraceHolder = JDKContext.Properties.COLLECT_FD_STACKTRACES ?
            new Exception("Resource Stack Trace") :
            null;

        Core.getJDKContext().register(this);
    }

    protected Exception getStackTraceHolder() {
        return stackTraceHolder;
    }

}
