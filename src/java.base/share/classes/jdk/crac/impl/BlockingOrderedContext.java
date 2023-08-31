/*
 * Copyright (c) 2019, 2021, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2019, 2021, Oracle and/or its affiliates. All rights reserved.
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

import jdk.crac.CheckpointException;
import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.RestoreException;
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
