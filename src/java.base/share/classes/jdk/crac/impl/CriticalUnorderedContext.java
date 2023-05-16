package jdk.crac.impl;

import jdk.crac.CheckpointException;
import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.RestoreException;

import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;

public class CriticalUnorderedContext<R extends Resource> extends AbstractContext<R> {
    private final WeakHashMap<R, Void> resources = new WeakHashMap<>();
    private ExceptionHolder<CheckpointException> exception = null;

    private synchronized List<R> snapshot() {
        return this.resources.entrySet().stream()
            .map(Map.Entry::getKey)
            .toList();
    }

    @Override
    protected synchronized List<R> checkpointSnapshot() {
        return snapshot();
    }
    @Override
    protected List<R> restoreSnapshot() {
        return snapshot();
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws CheckpointException {
        synchronized (this) {
            exception = new ExceptionHolder<>(CheckpointException::new);
        }

        try {
            super.beforeCheckpoint(context);
        } catch (CheckpointException e) {
            synchronized (this) {
                exception.handle(e);
            }
        }
        exception.throwIfAny();
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws RestoreException {
        synchronized (this) {
            exception = null;
        }
        super.afterRestore(context);
    }

    @Override
    public void register(R resource) {
        synchronized (this) {
            resources.put(resource, null);
            if (exception != null) {
                try {
                    invokeBeforeCheckpoint(resource);
                } catch (Exception e) {
                    exception.handle(e);
                }
            }
        }
    }
}
