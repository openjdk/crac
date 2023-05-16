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

import java.util.List;

public abstract class AbstractContext<R extends Resource> extends Context<R> {
    protected abstract List<R> checkpointSnapshot();
    protected abstract List<R> restoreSnapshot();

    protected void invokeBeforeCheckpoint(Resource resource) throws Exception {
        LoggerContainer.debug("beforeCheckpoint {0}", resource);
        resource.beforeCheckpoint(this);
    }

    protected void invokeAfterRestore(Resource resource) throws Exception {
        LoggerContainer.debug("afterRestore {0}", resource);
        resource.afterRestore(this);
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws CheckpointException {
        ExceptionHolder<CheckpointException> checkpointException =
            new ExceptionHolder<>(CheckpointException::new);
        List<R> resources = checkpointSnapshot();
        for (R r : resources) {
            checkpointException.runWithHandler(() -> {
                r.beforeCheckpoint(this);
            });
        }
        checkpointException.throwIfAny();
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws RestoreException {
        ExceptionHolder<RestoreException> restoreException =
            new ExceptionHolder<>(RestoreException::new);
        List<R> resources = restoreSnapshot();
        for (R r : resources) {
            restoreException.runWithHandler(() -> {
                invokeAfterRestore(r);
            });
        }
        restoreException.throwIfAny();
    }
}
