/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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

import java.util.*;

public abstract class PriorityContext<P, R extends Resource> extends AbstractContextImpl<R> {
    private final TreeMap<P, SubContext> categories;
    private final Comparator<P> comparator;
    private P lastPriority = null;

    protected PriorityContext(Comparator<P> comparator) {
        this.categories = new TreeMap<>(comparator);
        this.comparator = comparator;
    }

    protected synchronized void register(R resource, P priority) {
        while (lastPriority != null && comparator.compare(lastPriority, priority) >= 0) {
            waitWhileCheckpointIsInProgress(resource);
        }
        // computeIfAbsent does not work well here with lambda
        SubContext category = categories.get(priority);
        if (category == null) {
            category = new SubContext(getClass().getSimpleName() + "[" + priority + "]");
            categories.put(priority, category);
        }
        category.registerInSub(resource);
    }

    @Override
    protected void runBeforeCheckpoint() {
        Map.Entry<P, SubContext> entry;
        // We will use fine-grained synchronization to allow registration for higher category
        // in another thread.
        synchronized (this) {
            if (categories.isEmpty()) {
                return;
            }
            // This type of iteration should be O(N*log(N)), same as sorting, and does not suffer
            // from concurrent modifications. We'll track modifications for lower priorities in register()
            entry = categories.firstEntry();
            lastPriority = entry.getKey();
        }
        for (;;) {
            invokeBeforeCheckpoint(entry.getValue());
            synchronized (this) {
                entry = categories.higherEntry(entry.getKey());
                if (entry != null) {
                    lastPriority = entry.getKey();
                } else {
                    return;
                }
            }
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws RestoreException {
        synchronized (this) {
            lastPriority = null;
            notifyAll();
        }
        super.afterRestore(context);
    }

    public class SubContext extends OrderedContext<R> {
        public SubContext(String name) {
            super(name);
        }

        synchronized void registerInSub(R r) {
            resources.put(r, order++);
        }

        // This method differs from the super method only by the
        // parameter to the beforeCheckpoint method
        @Override
        protected void invokeBeforeCheckpoint(Resource resource) {
            LoggerContainer.debug("beforeCheckpoint {0}", resource);
            recordResource(resource);
            try {
                resource.beforeCheckpoint(PriorityContext.this);
            } catch (CheckpointException e) {
                handleCheckpointException(e);
            } catch (Exception e) {
                if (e instanceof InterruptedException) {
                    Thread.currentThread().interrupt();
                }
                ensureCheckpointException().addSuppressed(e);
            }
        }

        // This method differs from the super method only by the
        // parameter to the afterRestore method
        @Override
        protected void invokeAfterRestore(Resource resource) {
            LoggerContainer.debug("afterRestore {0}", resource);
            try {
                resource.afterRestore(PriorityContext.this);
            } catch (RestoreException e) {
                // Print error early in case the restore process gets stuck
                LoggerContainer.error(e, "Failed to restore " + resource);
                handleRestoreException(e);
            } catch (Exception e) {
                if (e instanceof InterruptedException) {
                    Thread.currentThread().interrupt();
                }
                LoggerContainer.error(e, "Failed to restore " + resource);
                ensureRestoreException().addSuppressed(e);
            }
        }
    }
}
