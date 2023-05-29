package jdk.crac.impl;

import jdk.crac.CheckpointException;
import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.RestoreException;

import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * Context performing Checkpoint notification in unspecified order.
 * Concurrent registration along beforeCheckpoint notification triggers
 * immediate notification on being registered resource.
 * @param <R>
 */
public class CriticalUnorderedContext<R extends Resource> extends AbstractContext<R> {
    private final WeakHashMap<R, Void> resources = new WeakHashMap<>();
    private ExceptionHolder<CheckpointException> concurrentCheckpointException = null;

    BlockingState blockingState = new BlockingState();

    private List<R> snapshot() {
        synchronized (blockingState) {
            return resources.entrySet().stream()
                .map(Map.Entry::getKey)
                .toList();
        }
    }

    @Override
    protected List<R> checkpointSnapshot() {
        return snapshot();
    }

    @Override
    protected List<R> restoreSnapshot() {
        // return updated set, as registration has called beforeCheckpoint()
        return snapshot();
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws CheckpointException {
        synchronized (blockingState) {
            concurrentCheckpointException = new ExceptionHolder<>(CheckpointException::new);
        }

        try {
            super.beforeCheckpoint(context);
        } catch (CheckpointException e) {
            synchronized (blockingState) {
                concurrentCheckpointException.handle(e);
            }
        }

        synchronized (blockingState) {
            blockingState.block();
            ExceptionHolder<CheckpointException> e = concurrentCheckpointException;
            concurrentCheckpointException = null;
            e.throwIfAny();
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws RestoreException {
        blockingState.unblock();
        super.afterRestore(context);
    }

    @Override
    public void register(R resource) {
        synchronized (blockingState) {
            blockingState.test();

            resources.put(resource, null);

            if (concurrentCheckpointException != null) {
                try {
                    invokeBeforeCheckpoint(resource);
                } catch (Exception e) {
                    concurrentCheckpointException.handle(e);
                }
            }
        }
    }
}
