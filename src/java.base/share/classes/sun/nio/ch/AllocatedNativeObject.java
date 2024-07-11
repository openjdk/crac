/*
 * Copyright (c) 2000, 2001, Oracle and/or its affiliates. All rights reserved.
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

/*
 */

package sun.nio.ch;                                     // Formerly in sun.misc

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKResource;
import jdk.internal.misc.Unsafe;

// ## In the fullness of time, this class will be eliminated

class AllocatedNativeObject                             // package-private
    extends NativeObject
{
    private final JDKResource resource;

    private void allocate(int size, boolean pageAligned) {
        if (!pageAligned) {
            this.allocationAddress = unsafe.allocateMemory(size);
            this.address = this.allocationAddress;
        } else {
            int ps = pageSize();
            long a = unsafe.allocateMemory(size + ps);
            this.allocationAddress = a;
            this.address = a + ps - (a & (ps - 1));
        }
    }

    /**
     * Allocates a memory area of at least {@code size} bytes outside of the
     * Java heap and creates a native object for that area.
     *
     * @param  size
     *         Number of bytes to allocate
     *
     * @param  pageAligned
     *         If {@code true} then the area will be aligned on a hardware
     *         page boundary
     *
     * @throws OutOfMemoryError
     *         If the request cannot be satisfied
     */
    AllocatedNativeObject(int size, boolean pageAligned) {
        allocate(size, pageAligned);
        resource = new JDKResource() {
            private byte[] data;

            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) {
                if (allocationAddress != 0) {
                    data = new byte[size];
                    unsafe.copyMemory(null, address, data, Unsafe.ARRAY_BYTE_BASE_OFFSET, size);
                    free();
                }
            };

            @Override
            public void afterRestore(Context<? extends Resource> context) {
                if (data != null) {
                    allocate(size, pageAligned);
                    unsafe.copyMemory(data, Unsafe.ARRAY_BYTE_BASE_OFFSET, null, address, size);
                    data = null;
                }
            };
        };
        Core.Priority.NORMAL.getContext().register(resource);
    }

    /**
     * Frees the native memory area associated with this object.
     */
    synchronized void free() {
        if (allocationAddress != 0) {
            unsafe.freeMemory(allocationAddress);
            allocationAddress = 0;
        }
    }

}
