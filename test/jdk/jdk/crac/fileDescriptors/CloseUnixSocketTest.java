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

import jdk.crac.Core;
import jdk.internal.crac.JDKFdResource;
import jdk.internal.crac.OpenResourcePolicies;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.net.*;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.CountDownLatch;

import static jdk.test.lib.Asserts.assertTrue;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "linux")
 * @build FDPolicyTestBase
 * @build CloseUnixSocketTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class CloseUnixSocketTest extends FDPolicyTestBase implements CracTest {
    @Override
    public void test() throws Exception {
        Path config = writeConfig("""
                type: SoCkEt
                action: close
                family: unix
                """);
        try {
            new CracBuilder()
                    .javaOption(JDKFdResource.COLLECT_FD_STACKTRACES_PROPERTY, "true")
                    .javaOption(OpenResourcePolicies.PROPERTY, config.toString())
                    .doCheckpointAndRestore();
        } finally {
            Files.deleteIfExists(config);
        }
    }

    @Override
    public void exec() throws Exception {
        Path socketFile = Files.createTempFile(CloseUnixSocketTest.class.getSimpleName(), ".socket");
        Files.deleteIfExists(socketFile);
        UnixDomainSocketAddress address = UnixDomainSocketAddress.of(socketFile);

        ServerSocketChannel serverChannel = ServerSocketChannel.open(StandardProtocolFamily.UNIX);
        serverChannel.bind(address);
        CountDownLatch latch1 = new CountDownLatch(1);
        CountDownLatch latch2 = new CountDownLatch(1);
        Thread serverThread = new Thread(() -> {
            try {
                SocketChannel socket = serverChannel.accept();
                latch1.countDown();
                // We need to prevent SocketChannel getting out of scope and being
                // garbage collected. When this happens the file descriptor leaks.
                // It is not up to CRaC to handle leaked descriptors.
                latch2.await();
            } catch (IOException | InterruptedException e) {
                e.printStackTrace();
            }
        });
        serverThread.setDaemon(true);
        serverThread.start();
        SocketChannel clientChannel = SocketChannel.open(StandardProtocolFamily.UNIX);
        assertTrue(clientChannel.connect(address));
        latch1.await();
        Core.checkpointRestore();
        latch2.countDown();
    }

}
