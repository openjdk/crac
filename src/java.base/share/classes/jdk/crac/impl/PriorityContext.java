// Copyright 2023 Azul Systems, Inc.
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

public abstract class PriorityContext<P, R extends Resource> extends AbstractContextImpl<R> {
    private final TreeMap<P, SubContext> categories;
    private final Comparator<P> comparator;
    private P lastPriority = null;

    protected PriorityContext(Comparator<P> comparator) {
        this.categories = new TreeMap<>(comparator);
        this.comparator = comparator;
    }

    protected synchronized void register(R resource, P priority) {
        // computeIfAbsent does not work well here with lambda
        SubContext category = categories.get(priority);
        if (category == null) {
            category = new SubContext(getClass().getSimpleName() + "." + priority);
            categories.put(priority, category);
        }
        category.registerInSub(resource);
        if (lastPriority != null && comparator.compare(lastPriority, priority) >= 0 && !Core.isRestoring()) {
            setModified(resource, ": resource priority " + priority + ", currently processing " + lastPriority);
        }
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
    public void afterRestore(Context<? extends Resource> context) {
        synchronized (this) {
            lastPriority = null;
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

        @Override
        protected Context<? extends Resource> semanticContext() {
            return PriorityContext.this;
        }
    }
}
