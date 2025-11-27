/*
 * Copyright (c) 2023, 2025 Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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

import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.channels.AsynchronousCloseException;
import java.nio.channels.AsynchronousServerSocketChannel;
import java.nio.channels.AsynchronousSocketChannel;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertTrue;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @build FDPolicyTestBase
 * @build ReopenListeningTestBase
 * @build ReopenListeningAsynchronousSocketChannelTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenListeningAsynchronousSocketChannelTest extends ReopenListeningTestBase<AsynchronousServerSocketChannel> implements CracTest {
    @Override
    protected AsynchronousServerSocketChannel createServer() throws IOException {
        return AsynchronousServerSocketChannel.open().bind(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0));
    }

    @Override
    protected boolean acceptClient(AsynchronousServerSocketChannel serverSocket) throws Exception {
        try {
            AsynchronousSocketChannel channel = serverSocket.accept().get();
            channel.close();
            return true;
        } catch (ExecutionException ee) {
            Throwable cause = ee.getCause();
            if (!(cause instanceof AsynchronousCloseException)) {
                assertTrue(cause instanceof IOException, ee.toString());
                assertTrue(ee.getMessage().contains("The I/O operation has been aborted because of either a thread exit or an application request"), ee.toString());
            }
            return false;
        }
    }

    @Override
    protected void connectClient(AsynchronousServerSocketChannel serverSocket) throws Exception {
        try (AsynchronousSocketChannel clientSocket = AsynchronousSocketChannel.open()) {
            Future<Void> connectFuture = clientSocket.connect(serverSocket.getLocalAddress());
            connectFuture.get();
        }
    }
}
