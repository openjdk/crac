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
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

package javax.crac;

import java.lang.ref.WeakReference;
import java.util.WeakHashMap;

class ResourceWrapper extends WeakReference<Resource> implements jdk.crac.Resource {
    private static WeakHashMap<Resource, ResourceWrapper> weakMap = new WeakHashMap<>();

    // Create strong reference to avoid losing the Resource.
    // It's set unconditionally in beforeCheckpoint and cleaned in afterRestore
    // (latter is called regardless of beforeCheckpoint result).
    private Resource strongRef;

    private final Context<Resource> context;

    public ResourceWrapper(Context<Resource> context, Resource resource) {
        super(resource);
        weakMap.put(resource, this);
        strongRef = null;
        this.context = context;
    }

    @Override
    public String toString() {
        return "ResourceWrapper[" + get().toString() + "]";
    }

    @Override
    public void beforeCheckpoint(jdk.crac.Context<? extends jdk.crac.Resource> context)
            throws Exception {
        Resource r = get();
        strongRef = r;
        if (r != null) {
            try {
                r.beforeCheckpoint(this.context);
            } catch (CheckpointException e) {
                Exception newException = new jdk.crac.CheckpointException();
                for (Throwable t : e.getSuppressed()) {
                    newException.addSuppressed(t);
                }
                throw newException;
            }
        }
    }

    @Override
    public void afterRestore(jdk.crac.Context<? extends jdk.crac.Resource> context) throws Exception {
        Resource r = get();
        strongRef = null;
        if (r != null) {
            try {
                r.afterRestore(this.context);
            } catch (RestoreException e) {
                Exception newException = new jdk.crac.RestoreException();
                for (Throwable t : e.getSuppressed()) {
                    newException.addSuppressed(t);
                }
                throw newException;
            }
        }
    }
}
