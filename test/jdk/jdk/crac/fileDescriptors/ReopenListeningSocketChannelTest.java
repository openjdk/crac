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
import jdk.test.lib.crac.CracTestArg;

import static jdk.test.lib.Asserts.*;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @build FDPolicyTestBase
 * @build ReopenListeningTestBase
 * @build ReopenListeningSocketChannelTest
 * @run driver jdk.test.lib.crac.CracTest true
 * @run driver jdk.test.lib.crac.CracTest false
 */
public class ReopenListeningSocketChannelTest extends ReopenListeningTestBase<ServerSocketChannel> implements CracTest {
    @CracTestArg
    boolean blocking;

    @Override
    protected ServerSocketChannel createServer() throws IOException {
        ServerSocketChannel channel = ServerSocketChannel.open();
        channel.configureBlocking(blocking);
        return channel.bind(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0));
    }

    @Override
    protected boolean acceptClient(ServerSocketChannel serverSocket) throws IOException {
        try {
            SocketChannel socket;
            if (blocking) {
                socket = serverSocket.accept();
                assertNotNull(socket);
            } else {
                while ((socket = serverSocket.accept()) == null) {
                    Thread.yield();
                }
            }
            socket.close();
            return true;
        } catch (ClosedChannelException e) {
            return false;
        }
    }

    @Override
    protected void connectClient(ServerSocketChannel serverSocket) throws Exception {
        try (SocketChannel clientSocket = SocketChannel.open(serverSocket.getLocalAddress())) {
            assertTrue(clientSocket.isConnected());
        }
    }
}
