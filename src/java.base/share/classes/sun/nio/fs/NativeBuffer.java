/*
 * Copyright (c) 2008, 2009, Oracle and/or its affiliates. All rights reserved.
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

package sun.nio.fs;

import java.lang.ref.Cleaner.Cleanable;
import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKResource;
import jdk.internal.misc.Unsafe;
import jdk.internal.ref.CleanerFactory;

/**
 * A light-weight buffer in native memory.
 */

class NativeBuffer implements AutoCloseable {
    private static final Unsafe unsafe = Unsafe.getUnsafe();

    private long address;
    private final int size;
    private Cleanable cleanable;

    // optional "owner" to avoid copying
    // (only safe for use by thread-local caches)
    private Object owner;

    private final JDKResource resource;

    private static class Deallocator implements Runnable {
        private final long address;
        Deallocator(long address) {
            this.address = address;
        }
        public void run() {
            unsafe.freeMemory(address);
        }
    }

    NativeBuffer(int size) {
        this.address = unsafe.allocateMemory(size);
        this.size = size;
        this.cleanable = CleanerFactory.cleaner()
                                       .register(this, new Deallocator(address));

        // TODO clear NativeBuffers from released buffers first so that unused
        //  data is not restored
        this.resource = new JDKResource() {
            private byte[] data;

            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) {
                if (address != 0) {
                    data = new byte[size];
                    unsafe.copyMemory(null, address, data, Unsafe.ARRAY_BYTE_BASE_OFFSET, size);
                    free();
                }
            };

            @Override
            public void afterRestore(Context<? extends Resource> context) {
                if (data != null) {
                    address = unsafe.allocateMemory(size);
                    cleanable = CleanerFactory.cleaner()
                                              .register(this, new Deallocator(address));
                    unsafe.copyMemory(data, Unsafe.ARRAY_BYTE_BASE_OFFSET, null, address, size);
                    data = null;
                }
            };
        };
        // Must have a priority higher than that of file descriptors because
        // used when reading file descriptor policies
        Core.Priority.POST_FILE_DESCRIPTORS.getContext().register(resource);
    }

    @Override
    public void close() {
        release();
    }

    void release() {
        NativeBuffers.releaseNativeBuffer(this);
    }

    long address() {
        return address;
    }

    int size() {
        return size;
    }

    void free() {
        cleanable.clean();
        address = 0;
    }

    // not synchronized; only safe for use by thread-local caches
    void setOwner(Object owner) {
        this.owner = owner;
    }

    // not synchronized; only safe for use by thread-local caches
    Object owner() {
        return owner;
    }
}
