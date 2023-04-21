// Copyright 2019-2020 Azul Systems, Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

package jdk.crac.impl;

import jdk.crac.*;

import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;

public abstract class AbstractContextImpl<R extends Resource> extends Context<R> {
    private WeakHashMap<R, Long> resources = new WeakHashMap<>();
    // Queue content is temporary, so we won't mind that it's not a weak reference
    private Queue<Map.Entry<R, Long>> resourceQueue = new LinkedList<>();
    private List<R> restoreQ = null;
    private volatile long currentPriority = -1;
    // We use two locks: checkpointLock is required for both running the beforeCheckpoint
    // and registering a new resource, while restoreLock is required for running afterRestore
    // and beforeCheckpoint (to achieve exclusivity of before and after). It is fine
    // to acquire checkpointLock and register a new resource during afterRestore.
    private final ReentrantLock checkpointLock = new ReentrantLock();
    private final ReentrantLock restoreLock = new ReentrantLock();

    protected void register(R resource, long priority) {
        assert priority >= 0;
        boolean locked = false;
        try {
            // We don't want to deadlock if the registration happens from another thread
            while (!checkpointLock.tryLock(10, TimeUnit.MILLISECONDS)) {
                throwIfCheckpointInProgress(priority);
            }
            locked = true;
            // This is important for the case of recursive registration
            throwIfCheckpointInProgress(priority);
            if (currentPriority < 0) {
                resources.put(resource, priority);
            } else {
                resourceQueue.add(Map.entry(resource, priority));
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            if (locked) {
                checkpointLock.unlock();
            }
        }
    }

    private void throwIfCheckpointInProgress(long priority) {
        if (priority <= currentPriority) {
            throw new IllegalStateException("Notifications for an upcoming checkpoint are already in progress (priority "
                    + currentPriority + "). Please make sure to register this resource earlier or use higher priorty (" + priority + ")");
        }
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws CheckpointException {
        // If afterRestore is running we need to delay the beforeCheckpoint
        restoreLock.lock();
        try {
            checkpointLock.lock();
            try {
                runBeforeCheckpoint();
            } finally {
                if (restoreQ != null) {
                    Collections.reverse(restoreQ);
                }
                currentPriority = -1;
                checkpointLock.unlock();
            }
        } finally {
            restoreLock.unlock();
        }
    }

    private void runBeforeCheckpoint() throws CheckpointException {
        Map.Entry<R, Long> drained;
        while ((drained = resourceQueue.poll()) != null) {
            resources.put(drained.getKey(), drained.getValue());
        }
        CheckpointException exception = null;
        TreeMap<Long, List<R>> resources = this.resources.entrySet().stream().collect(
                TreeMap::new, (m, e) -> m.computeIfAbsent(e.getValue(), p -> new ArrayList<>()).add(e.getKey()), TreeMap::putAll);
        restoreQ = new ArrayList<>(this.resources.size());

        // We cannot simply iterate because we could cause mutations
        while (!resources.isEmpty()) {
            var entry = resources.firstEntry();
            resources.remove(entry.getKey());
            currentPriority = entry.getKey();
            for (R r : entry.getValue()) {
                LoggerContainer.debug("beforeCheckpoint {0}", r);
                try {
                    r.beforeCheckpoint(this);
                    restoreQ.add(r);
                } catch (CheckpointException e) {
                    enqueueIfContext(r);
                    if (exception == null) {
                        exception = new CheckpointException();
                    }
                    Throwable[] suppressed = e.getSuppressed();
                    if (suppressed.length == 0) {
                        exception.addSuppressed(e);
                    }
                    for (Throwable t : suppressed) {
                        exception.addSuppressed(t);
                    }
                } catch (Exception e) {
                    enqueueIfContext(r);
                    if (exception == null) {
                        exception = new CheckpointException();
                    }
                    exception.addSuppressed(e);
                }
            }
            while ((drained = resourceQueue.poll()) != null) {
                if (drained.getValue() <= currentPriority) {
                    // this should be prevented in register method
                    throw new IllegalStateException();
                }
                resources.computeIfAbsent(drained.getValue(), p -> new ArrayList<>()).add(drained.getKey());
            }
        }

        if (exception != null) {
            throw exception;
        }
    }

    private void enqueueIfContext(R r) {
        // When the resource itself is a context it contains other resources that should
        // be restored upon unsuccessful checkpoint (if the beforeCheckpoint has thrown
        // we are going to call afterRestore on all checkpointed resources immediately)
        if (r instanceof Context<?>) {
            restoreQ.add(r);
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws RestoreException {
        restoreLock.lock();
        try {
            RestoreException exception = null;
            for (Resource r : restoreQ) {
                LoggerContainer.debug("afterRestore {0}", r);
                try {
                    r.afterRestore(this);
                } catch (RestoreException e) {
                    // Print error early in case the restore process gets stuck
                    LoggerContainer.error(e, "Failed to restore " + r);
                    if (exception == null) {
                        exception = new RestoreException();
                    }
                    Throwable[] suppressed = e.getSuppressed();
                    if (suppressed.length == 0) {
                        exception.addSuppressed(e);
                    }
                    for (Throwable t : suppressed) {
                        exception.addSuppressed(t);
                    }
                } catch (Exception e) {
                    // Print error early in case the restore process gets stuck
                    LoggerContainer.error(e, "Failed to restore " + r);
                    if (exception == null) {
                        exception = new RestoreException();
                    }
                    exception.addSuppressed(e);
                }
            }
            restoreQ = null;

            if (exception != null) {
                throw exception;
            }
        } finally {
            restoreLock.unlock();
        }
    }
}
