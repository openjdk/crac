package jdk.internal.crac.mirror.impl;

import jdk.internal.crac.mirror.CheckpointException;
import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;
import jdk.internal.crac.mirror.RestoreException;
import jdk.internal.crac.LoggerContainer;

public class BlockingOrderedContext<R extends Resource> extends OrderedContext<R> {
    private boolean checkpointing = false;

    // This method has particularly verbose name to stick out in thread dumps
    // when the registration leads to a deadlock.
    private void waitWhileCheckpointIsInProgress(R resource) {
        if (Thread.currentThread().isInterrupted()) {
            // FIXME this block effectively translates interrupted status to RuntimeException
            LoggerContainer.debug(Thread.currentThread().getName() + " not waiting in " + this +
                " to register " + resource + "; the thread has already been interrupted.");
            // We won't cause IllegalStateException because this is not an unexpected state
            // from the point of CRaC - it probably tried to register some code before.
            throw new RuntimeException("Interrupted thread tried to block in registration of " + resource + " in " + this);
        }
        LoggerContainer.debug(Thread.currentThread().getName() + " waiting in " + this + " to register " + resource);
        try {
            wait();
        } catch (InterruptedException e) {
            // FIXME there should be no interrupt once we've got interrupted
            Thread.currentThread().interrupt();
            LoggerContainer.debug(Thread.currentThread().getName() + " interrupted waiting in " + this +
                " to register " + resource);
            throw new RuntimeException("Interrupted while trying to register " + resource + " in " + this, e);
        }
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws CheckpointException {
        synchronized (this) {
            checkpointing = true;
        }
        super.beforeCheckpoint(context);
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws RestoreException {
        // unblock all registrations before running afterRestore()'s
        synchronized (this) {
            checkpointing = false;
            notifyAll();
        }
        super.afterRestore(context);
    }

    @Override
    public void register(R resource) {
        synchronized (this) {
            while (checkpointing) {
                waitWhileCheckpointIsInProgress(resource);
            }
            super.register(resource);
        }
    }
}
