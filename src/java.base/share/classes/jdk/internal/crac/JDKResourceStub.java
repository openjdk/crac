package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;

public class JDKResourceStub implements JDKResource {
    private final JDKResource.Priority priority;

    protected JDKResourceStub(Priority priority) {
        this.priority = priority;
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
    }

    @Override
    public Priority getPriority() {
        return priority;
    }
}
