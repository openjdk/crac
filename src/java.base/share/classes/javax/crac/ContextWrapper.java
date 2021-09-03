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

class ContextWrapper extends Context<Resource> {
    private final jdk.crac.Context<jdk.crac.Resource> context;

    public ContextWrapper(jdk.crac.Context<jdk.crac.Resource> context) {
        this.context = context;
    }

    private static jdk.crac.Context<? extends jdk.crac.Resource> convertContext(
            Context<? extends Resource> context) {
        return context instanceof ContextWrapper ?
                ((ContextWrapper)context).context :
                null;
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context)
            throws CheckpointException {
        try {
            this.context.beforeCheckpoint(convertContext(context));
        } catch (jdk.crac.CheckpointException e) {
            CheckpointException newException = new CheckpointException();
            for (Throwable t : e.getSuppressed()) {
                newException.addSuppressed(t);
            }
            throw newException;
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context)
            throws RestoreException {
        try {
            this.context.afterRestore(convertContext(context));
        } catch (jdk.crac.RestoreException e) {
            RestoreException newException = new RestoreException();
            for (Throwable t : e.getSuppressed()) {
                newException.addSuppressed(t);
            }
            throw newException;
        }
    }

    @Override
    public void register(Resource r) {
        ResourceWrapper wrapper = new ResourceWrapper(this, r);
        context.register(wrapper);
    }

    @Override
    public String toString() {
        return "ContextWrapper[" + context.toString() + "]";
    }
}

