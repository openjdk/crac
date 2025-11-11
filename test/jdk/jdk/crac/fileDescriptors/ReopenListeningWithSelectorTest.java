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

import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.IOException;
import java.nio.channels.*;
import java.nio.channels.spi.SelectorProvider;

import static jdk.test.lib.Asserts.*;

/**
 * @test id=DEFAULT
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @build FDPolicyTestBase
 * @build ReopenListeningTestBase
 * @build ReopenListeningSocketChannelTest
 * @build ReopenListeningWithSelectorTest
 * @run driver jdk.test.lib.crac.CracTest false
 */
/**
 * @test id=ALT_LINUX
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "linux")
 * @build FDPolicyTestBase
 * @build ReopenListeningTestBase
 * @build ReopenListeningSocketChannelTest
 * @build ReopenListeningWithSelectorTest
 * @run driver jdk.test.lib.crac.CracTest false sun.nio.ch.PollSelectorProvider
 */
/**
 * @test id=ALT_WINDOWS
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "windows")
 * @build FDPolicyTestBase
 * @build ReopenListeningTestBase
 * @build ReopenListeningSocketChannelTest
 * @build ReopenListeningWithSelectorTest
 * @run driver jdk.test.lib.crac.CracTest false sun.nio.ch.WindowsSelectorProvider
 */
public class ReopenListeningWithSelectorTest extends ReopenListeningSocketChannelTest implements CracTest {
    private Selector selector;

    @CracTestArg(value = 1, optional = true)
    String selectorImpl;

    @Override
    protected CracBuilder builder() {
        CracBuilder builder = super.builder();
        if (selectorImpl != null) {
            builder.javaOption(SelectorProvider.class.getName(), selectorImpl);
        }
        return builder;
    }

    @Override
    public void exec() throws Exception {
        if (selectorImpl != null) {
            assertEquals(selectorImpl, SelectorProvider.provider().getClass().getName());
        }
        selector = Selector.open();
        super.exec();
        selector.close();
    }

    @Override
    protected ServerSocketChannel createServer() throws IOException {
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
