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
import jdk.internal.crac.LoggerContainer;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public abstract class AbstractContextImpl<R extends Resource> extends Context<R> {
    private List<Resource> restoreQ = null;
    private CheckpointException checkpointException = null;
    private RestoreException restoreException = null;

    protected void invokeBeforeCheckpoint(Resource resource) {
        LoggerContainer.debug("beforeCheckpoint {0}", resource);
        recordResource(resource);
        try {
            resource.beforeCheckpoint(this);
        } catch (CheckpointException e) {
            CheckpointException ce = ensureCheckpointException();
            for (Throwable t : e.getSuppressed()) {
                ce.addSuppressed(t);
            }
        } catch (Exception e) {
            if (e instanceof InterruptedException) {
                Thread.currentThread().interrupt();
            }
            ensureCheckpointException().addSuppressed(e);
        }
    }

    protected CheckpointException ensureCheckpointException() {
        if (checkpointException == null) {
            checkpointException = new CheckpointException();
        }
        return checkpointException;
    }

    protected void recordResource(Resource resource) {
        // Resource.afterRestore is invoked even if Resource.beforeCheckpoint fails
        restoreQ.add(resource);
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws CheckpointException {
        // We won't synchronize access to restoreQ because methods
        // beforeCheckpoint and afterRestore should be invoked only
        // by the single thread performing the C/R and other threads should
        // not touch that.
        restoreQ = new ArrayList<>();
        runBeforeCheckpoint();
        Collections.reverse(restoreQ);
        if (checkpointException != null) {
            CheckpointException ce = checkpointException;
            checkpointException = null;
            throw ce;
        }
    }

    // This method has particularly verbose name to stick out in thread dumps
    // when the registration leads to a deadlock.
    protected void waitWhileCheckpointIsInProgress(R resource) {
        if (Thread.currentThread().isInterrupted()) {
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
            Thread.currentThread().interrupt();
            LoggerContainer.debug(Thread.currentThread().getName() + " interrupted waiting in " + this +
                    " to register " + resource);
            throw new RuntimeException("Interrupted while trying to register " + resource + " in " + this, e);
        }
    }

    protected abstract void runBeforeCheckpoint();

    @Override
    public void afterRestore(Context<? extends Resource> context) throws RestoreException {
        List<Resource> queue = restoreQ;
        if (queue == null) {
            return;
        }
        restoreQ = null;
        for (Resource r : queue) {
            invokeAfterRestore(r);
        }
        if (restoreException != null) {
            RestoreException re = restoreException;
            restoreException = null;
            throw re;
        }
    }

    protected void invokeAfterRestore(Resource resource) {
        LoggerContainer.debug("afterRestore {0}", resource);
        try {
            resource.afterRestore(this);
        } catch (RestoreException e) {
            // Print error early in case the restore process gets stuck
            LoggerContainer.error(e, "Failed to restore " + resource);
            RestoreException re = ensureRestoreException();
            for (Throwable t : e.getSuppressed()) {
                re.addSuppressed(t);
            }
        } catch (Exception e) {
            if (e instanceof InterruptedException) {
                Thread.currentThread().interrupt();
            }
            LoggerContainer.error(e, "Failed to restore " + resource);
            ensureRestoreException().addSuppressed(e);
        }
    }

    protected RestoreException ensureRestoreException() {
        if (restoreException == null) {
            restoreException = new RestoreException();
        }
        return restoreException;
    }

}
