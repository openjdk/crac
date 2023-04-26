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
    protected List<Resource> restoreQ = null;

    protected static <E extends Exception> void recordExceptions(E source) {
        Throwable[] suppressed = source.getSuppressed();
        if (suppressed.length == 0) {
            Core.recordException(source);
        }
        for (Throwable t : suppressed) {
            Core.recordException(t);
        }
    }

    protected void setModified(R resource, String msg) {
        Core.recordException(new CheckpointException(
                "Adding resource " + resource + " to " + this + (msg != null ? msg : "")));
    }

    protected void invokeBeforeCheckpoint(Resource resource) {
        LoggerContainer.debug("beforeCheckpoint {0}", resource);
        // Resource.afterRestore is invoked even if Resource.beforeCheckpoint fails
        restoreQ.add(resource);
        try {
            resource.beforeCheckpoint(semanticContext());
        } catch (CheckpointException e) {
            recordExceptions(e);
        } catch (Exception e) {
            Core.recordException(e);
        }
    }

    protected Context<? extends Resource> semanticContext() {
        return this;
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) {
        // We won't synchronize access to restoreQ because methods
        // beforeCheckpoint and afterRestore should be invoked only
        // by the single thread performing the C/R and other threads should
        // not touch that.
        restoreQ = new ArrayList<>();
        runBeforeCheckpoint();
        Collections.reverse(restoreQ);
    }

    protected abstract void runBeforeCheckpoint();

    @Override
    public void afterRestore(Context<? extends Resource> context) {
        List<Resource> queue = restoreQ;
        restoreQ = null;
        runAfterRestore(queue);
    }

    private void runAfterRestore(List<Resource> queue) {
        if (queue == null) {
            return;
        }
        for (Resource r : queue) {
            LoggerContainer.debug("afterRestore {0}", r);
            try {
                r.afterRestore(semanticContext());
            } catch (RestoreException e) {
                // Print error early in case the restore process gets stuck
                LoggerContainer.error(e, "Failed to restore " + r);
                recordExceptions(e);
            } catch (Exception e) {
                // Print error early in case the restore process gets stuck
                LoggerContainer.error(e, "Failed to restore " + r);
                Core.recordException(e);
            }
        }
    }
}
