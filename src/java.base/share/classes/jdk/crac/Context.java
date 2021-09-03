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

package jdk.crac;

/**
 * A {@code Resource} that allows other {@code Resource}s to be registered with it.
 *
 * <p>{@code Context} implementation overrides {@code beforeCheckpoint} and {@code afterRestore}, defining how the notification about checkpoint and restore will be distributed by the {@code Context} hierarchy.
 *
 * <p>A {@code Context} implementor is encouraged to respect properties of the global {@code Context}.
 */
public abstract class Context<R extends Resource> implements Resource {

    /** Creates a {@code Context}.
     */
    protected Context() {
    }

    @Override
    public abstract void beforeCheckpoint(Context<? extends Resource> context)
            throws CheckpointException;

    @Override
    public abstract void afterRestore(Context<? extends Resource> context)
            throws RestoreException;

    /**
     * Registers a {@code Resource} with this {@code Context}.
     *
     * @param resource {@code Resource} to be registered.
     * @throws NullPointerException if {@code resource} is {@code null}
     */
    public abstract void register(R resource);
}
