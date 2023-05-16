/*
 * Copyright (c) 2007, 2019, Oracle and/or its affiliates. All rights reserved.
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
package java.net;

import java.io.IOException;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.JDKResource;

/*
 * On Unix systems we simply delegate to native methods.
 *
 * @author Chris Hegarty
 */

class PlainSocketImpl extends AbstractPlainSocketImpl
{
    static class ResourceProxy implements JDKResource {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            PlainSocketImpl.beforeCheckpoint();
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) throws Exception {
            PlainSocketImpl.afterRestore();
        }
    }

    static Object closeLock = new Object();
    static boolean forceNonDeferedClose;
    static int closeCnt;

    static JDKResource resourceProxy = new ResourceProxy();

    static {
        initProto();
        JDKResource.Priority.NORMAL.getContext().register(resourceProxy);
    }

    /**
     * Constructs an empty instance.
     */
    PlainSocketImpl(boolean isServer) {
        super(isServer);
    }

    protected void socketSetOption(int opt, boolean b, Object val) throws SocketException {
        if (opt == SocketOptions.SO_REUSEPORT &&
            !supportedOptions().contains(StandardSocketOptions.SO_REUSEPORT)) {
            throw new UnsupportedOperationException("unsupported option");
        }
        try {
            socketSetOption0(opt, b, val);
        } catch (SocketException se) {
            if (!isConnected)
                throw se;
        }
    }

    @Override
    void socketClose0(boolean useDeferredClose) throws IOException {
        if (useDeferredClose) {
            synchronized (closeLock) {
                if (forceNonDeferedClose) {
                    useDeferredClose = false;
                }
                if (useDeferredClose) {
                    ++closeCnt;
                }
            }
        }

        try {
            socketClose1(useDeferredClose);
        } finally {
            if (useDeferredClose) {
                synchronized (closeLock) {
                    --closeCnt;
                    if (forceNonDeferedClose && closeCnt == 0) {
                        closeLock.notifyAll();
                    }
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

    static void afterRestore() {
        synchronized (closeLock) {
            afterRestore0();
            forceNonDeferedClose = false;
        }
    }

    void socketCreate(boolean stream) throws IOException {
        socketCreate(stream, isServer);
    }

    native void socketCreate(boolean stream, boolean isServer) throws IOException;

    native void socketConnect(InetAddress address, int port, int timeout)
        throws IOException;

    native void socketBind(InetAddress address, int port)
        throws IOException;

    native void socketListen(int count) throws IOException;

    native void socketAccept(SocketImpl s) throws IOException;

    native int socketAvailable() throws IOException;

    native void socketClose1(boolean useDeferredClose) throws IOException;

    native void socketShutdown(int howto) throws IOException;

    static native void initProto();

    native void socketSetOption0(int cmd, boolean on, Object value)
        throws SocketException;

    native int socketGetOption(int opt, Object iaContainerObj) throws SocketException;

    native void socketSendUrgentData(int data) throws IOException;

    static native void beforeCheckpoint0();

    static native void afterRestore0();
}
