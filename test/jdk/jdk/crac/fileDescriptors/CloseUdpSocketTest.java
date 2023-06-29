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
import jdk.internal.crac.OpenResourcePolicies;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.net.*;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.CountDownLatch;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @build FDPolicyTestBase
 * @build CloseUdpSocketTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class CloseUdpSocketTest extends FDPolicyTestBase implements CracTest {
    @Override
    public void test() throws Exception {
        String loopback = InetAddress.getLoopbackAddress().getHostAddress();
        Path config = writeConfig("""
                type: socket
                localAddress: $loopback
                action: close
                """.replace("$loopback", loopback));
        try {
            new CracBuilder()
                    .javaOption(OpenResourcePolicies.PROPERTY, config.toString())
                    .doCheckpointAndRestore();
        } finally {
            Files.deleteIfExists(config);
        }
    }

    @Override
    public void exec() throws Exception {
        try (DatagramSocket serverSocket = new DatagramSocket(0, InetAddress.getLoopbackAddress())) {
            CountDownLatch latch = new CountDownLatch(1);
            Thread serverThread = new Thread(() -> {
                try {
                    byte[] buf = new byte[1024];
                    DatagramPacket packet = new DatagramPacket(buf, buf.length);
                    serverSocket.receive(packet);
                    latch.countDown();
                } catch (IOException e) {
                    e.printStackTrace();
                }
            });
            serverThread.setDaemon(true);
            serverThread.start();
            try (DatagramSocket clientSocket = new DatagramSocket()) {
                clientSocket.connect(InetAddress.getLoopbackAddress(), serverSocket.getLocalPort());
                byte[] buf = "Hello".getBytes();
                clientSocket.send(new DatagramPacket(buf, buf.length));
                latch.await();
                Core.checkpointRestore();
            }
        }
    }
}
