/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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
import java.nio.channels.*;

import static jdk.test.lib.Asserts.*;

// FIXME: JDK-8371549 - remove @requires Linux
/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "linux")
 * @build FDPolicyTestBase
 * @build ReopenListeningTestBase
 * @build ReopenListeningSocketChannelTest
 * @build ReopenListeningWithSelectorTest
 * @run driver/timeout=10 jdk.test.lib.crac.CracTest false
 */
public class ReopenListeningWithSelectorTest extends ReopenListeningSocketChannelTest implements CracTest {
    private Selector selector;

    @Override
    public void exec() throws Exception {
        super.exec();
        selector.close();
    }

    @Override
    protected ServerSocketChannel createServer() throws IOException {
        selector = Selector.open();
        ServerSocketChannel channel = super.createServer();
        channel.register(selector, SelectionKey.OP_ACCEPT);
        return channel;
    }

    @Override
    protected boolean acceptClient(ServerSocketChannel serverSocket) throws IOException {
        if (selector.select() == 0) {
            return false;
        }
        for (SelectionKey key : selector.selectedKeys()) {
            assertTrue(key.isAcceptable());
            assertEquals(serverSocket, key.channel());
            SocketChannel socket = serverSocket.accept();
            socket.close();
        }
        selector.selectedKeys().clear();
        return true;
    }

}
