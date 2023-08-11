/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

package sun.nio.ch;

import java.io.FileDescriptor;
import java.io.IOException;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKResource;

abstract class UnixDispatcher extends NativeDispatcher {

    static class ResourceProxy implements JDKResource {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            UnixDispatcher.beforeCheckpoint();
        }

        @Override
        public void afterRestore(Context<? extends Resource> context)
                throws IOException {
            UnixDispatcher.afterRestore();
        }
    }

    static Object closeLock = new Object();
    static boolean forceNonDeferedClose;
    static int closeCnt;

    static ResourceProxy resourceProxy = new ResourceProxy();


    void close(FileDescriptor fd) throws IOException {
        close0(fd);
    }

    void preClose(FileDescriptor fd) throws IOException {
        boolean doPreclose = true;
        synchronized (closeLock) {
            if (forceNonDeferedClose) {
                doPreclose = false;
            }
            if (doPreclose) {
                ++closeCnt;
            }
        }

        if (!doPreclose) {
            return;
        }

        try {
            preClose0(fd);
        } finally {
            synchronized (closeLock) {
                closeCnt--;
                if (forceNonDeferedClose && closeCnt == 0) {
                    closeLock.notifyAll();
                }
            }
        }
    }

    static void beforeCheckpoint() throws InterruptedException {
        synchronized (closeLock) {
            forceNonDeferedClose = true;
            while (closeCnt != 0) {
                closeLock.wait();
            }
            beforeCheckpoint0();
        }
    }

    static void afterRestore() throws IOException {
        synchronized (closeLock) {
            afterRestore0();
            forceNonDeferedClose = false;
        }
    }

    private static final JavaIOFileDescriptorAccess fdAccess =
        SharedSecrets.getJavaIOFileDescriptorAccess();

    static void closeAndMark(FileDescriptor fd) throws IOException {
        // Originally this used fdAccess.markClosed() and close0() but leaving
        // the FD value set breaks JDKSocketResource (we don't want the extra
        // test if the FD resource has been marked).
        fdAccess.close(fd);
    }

    private static native void close0(FileDescriptor fd) throws IOException;

    static native void preClose0(FileDescriptor fd) throws IOException;

    static native void init();

    static native void beforeCheckpoint0();

    static native void afterRestore0() throws IOException;

    static {
        IOUtil.load();
        init();
        // We cannot register using normal priority because other JDK resources
        // might read configuration files with this or later priority.
        // It's difficult to trigger static initialization outside the package.
        Core.Priority.PRE_FILE_DESCRIPTORS.getContext().register(resourceProxy);
    }
}
