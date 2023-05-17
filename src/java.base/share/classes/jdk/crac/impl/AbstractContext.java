/*
 * Copyright (c) 2019, 2023, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package jdk.crac.impl;

import jdk.crac.*;
import jdk.internal.crac.LoggerContainer;

import java.util.List;

/**
 * An abstract context with few utilities.
 * @param <R> Type of Resource managed by the context.
 */
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
                invokeBeforeCheckpoint(r);
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
