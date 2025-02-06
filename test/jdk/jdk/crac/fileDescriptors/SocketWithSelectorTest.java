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

import jdk.crac.CheckpointException;
import jdk.crac.Core;
import jdk.internal.crac.OpenResourcePolicies;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "linux")
 * @build FDPolicyTestBase
 * @build SocketWithSelectorTest
 * @run driver jdk.test.lib.crac.CracTest true
 * @run driver jdk.test.lib.crac.CracTest false
 */
public class SocketWithSelectorTest extends FDPolicyTestBase implements CracTest {
    @CracTestArg
    boolean closeSocket;

    @Override
    public void test() throws Exception {
        String loopback = InetAddress.getLoopbackAddress().getHostAddress();
        Path config = writeConfig("""
                type: SOCKET
                family: ip
                localAddress: $loopback
                action: close
                """.replace("$loopback", loopback));
        try {
            CracBuilder builder = new CracBuilder();
            if (closeSocket) {
                builder.javaOption(OpenResourcePolicies.PROPERTY, config.toString());
            }
            CracProcess checkpointed = builder.startCheckpoint();
            if (closeSocket) {
                checkpointed.waitForCheckpointed();
                builder.doRestore();
            } else {
                // the checkpoint is supposed to fail
                checkpointed.waitForSuccess();
            }
        } finally {
            Files.deleteIfExists(config);
        }
    }

    @Override
    public void exec() throws Exception {
        Selector selector = Selector.open();
        ServerSocketChannel serverChannel = ServerSocketChannel.open()
                .bind(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0));
        serverChannel.configureBlocking(false);
        serverChannel.register(selector, SelectionKey.OP_ACCEPT);
        try {
            Core.checkpointRestore();
        } catch (CheckpointException e) {
            assertEquals(1L, Arrays.stream(e.getSuppressed()).filter(e2 -> e2.getMessage().contains("has registered keys from channels")).count());
            assertEquals(2L, Arrays.stream(e.getSuppressed()).filter(e2 -> e2.getMessage().contains("with registered keys")).count());
            assertEquals(1L, Arrays.stream(e.getSuppressed()).filter(e2 -> e2.getClass().getSimpleName().equals("CheckpointOpenSocketException"))
                    .filter(e2 -> e2.getMessage().contains("ServerSocketChannelImpl")).count());
        }
    }
}
