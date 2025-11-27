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

import jdk.test.lib.Asserts;
import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @build FDPolicyTestBase
 * @build ReopenListeningTestBase
 * @build ReopenListeningSocketTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenListeningSocketTest extends ReopenListeningTestBase<ServerSocket> implements CracTest {
    @Override
    protected ServerSocket createServer() throws IOException {
        return new ServerSocket(0, 50, InetAddress.getLoopbackAddress());
    }

    @Override
    protected boolean acceptClient(ServerSocket serverSocket) throws Exception {
        try {
            Socket socket = serverSocket.accept();
            socket.close();
            return true;
        } catch (SocketException e) {
            assertEquals("Socket closed", e.getMessage());
            return false;
        }
    }

    @Override
    protected void connectClient(ServerSocket serverSocket) throws Exception {
        try (Socket clientSocket = new Socket(InetAddress.getLoopbackAddress(), serverSocket.getLocalPort())) {
            Asserts.assertTrue(clientSocket.isConnected());
        }
    }
}
