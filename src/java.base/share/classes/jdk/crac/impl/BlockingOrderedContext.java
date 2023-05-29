package jdk.crac.impl;

import jdk.crac.CheckpointException;
import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.RestoreException;

public class BlockingOrderedContext<R extends Resource> extends OrderedContext<R> {
    private final BlockingState blockingState = new BlockingState();

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws CheckpointException {
        blockingState.block();
        super.beforeCheckpoint(context);
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws RestoreException {
        // unblock all registrations before running afterRestore()'s
        blockingState.unblock();
        super.afterRestore(context);
    }

    @Override
    public void register(R resource) {
        synchronized (blockingState) {
            blockingState.test();
            super.register(resource);
        }
    }
}
