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
import jdk.crac.impl.OpenFDPolicies;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.concurrent.CountDownLatch;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.crac.impl:+open
 * @build FDPolicyTestBase
 * @build CloseSocketTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class CloseSocketTest extends FDPolicyTestBase implements CracTest {
    @Override
    public void test() throws Exception {
        String checkpointPolicies = "SOCKET=" + OpenFDPolicies.BeforeCheckpoint.CLOSE;
        new CracBuilder()
                .javaOption(OpenFDPolicies.CHECKPOINT_PROPERTY, checkpointPolicies)
                .javaOption(OpenFDPolicies.RESTORE_PROPERTY, "SOCKET=" + OpenFDPolicies.AfterRestore.KEEP_CLOSED)
                .doCheckpointAndRestore();
    }

    @Override
    public void exec() throws Exception {
        ServerSocket serverSocket = new ServerSocket(0, 50, InetAddress.getLoopbackAddress());
        CountDownLatch latch = new CountDownLatch(1);
        Thread serverThread = new Thread(() -> {
            try {
                Socket socket = serverSocket.accept();
                latch.countDown();
            } catch (IOException e) {
                e.printStackTrace();
            }
        });
        serverThread.setDaemon(true);
        serverThread.start();
        Socket clientSocket = new Socket(InetAddress.getLoopbackAddress(), serverSocket.getLocalPort());
        latch.await();
        Core.checkpointRestore();
        System.out.println("Not much to do here");
    }

}
