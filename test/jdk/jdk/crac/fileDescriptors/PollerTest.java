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
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.concurrent.atomic.AtomicReference;

/**
 * @test
 * @library /test/lib
 * @build PollerTest
 * @run driver/timeout=10 jdk.test.lib.crac.CracTest false VTHREAD_POLLERS
 * @run driver/timeout=10 jdk.test.lib.crac.CracTest true VTHREAD_POLLERS
 * @run driver/timeout=10 jdk.test.lib.crac.CracTest false SYSTEM_THREADS
 * @run driver/timeout=10 jdk.test.lib.crac.CracTest true SYSTEM_THREADS
 */
public class PollerTest implements CracTest {
    @CracTestArg(0)
    boolean checkpointVirtual;

    @CracTestArg(1)
    String pollerMode;

    @Override
    public void test() throws Exception {
        new CracBuilder()
                .engine(CracEngine.SIMULATE)
                .javaOption("jdk.pollerMode", pollerMode)
                .startCheckpoint().waitForSuccess();
    }

    @FunctionalInterface
    interface ThrowingRunnable {
        public void run() throws Exception;
    }

    @Override
    public void exec() throws Exception {
        execInVirtual(this::useSockets);
        if (checkpointVirtual) {
            execInVirtual(Core::checkpointRestore);
        } else {
            Core.checkpointRestore();
        }
        execInVirtual(this::useSockets);
    }

    public void execInVirtual(ThrowingRunnable runnable) throws Exception {
        AtomicReference<Exception> ex = new AtomicReference<>();
        Thread.startVirtualThread(() -> {
            try {
                runnable.run();
            } catch (Exception e) {
                ex.set(e);
            }
        }).join();
        if (ex.get() != null) {
            throw ex.get();
        }
    }

    private void useSockets() throws IOException, InterruptedException {
        ServerSocket serverSocket = new ServerSocket(0, 50, InetAddress.getLoopbackAddress());
        Thread serverThread = Thread.startVirtualThread(() -> {
            try {
                Socket socket = serverSocket.accept();
                socket.close();
                // the socket leaks in here but for some reason it does not leave the FD open
            } catch (IOException e) {
                e.printStackTrace();
            }
        });
        Socket clientSocket = new Socket(InetAddress.getLoopbackAddress(), serverSocket.getLocalPort());
        clientSocket.close();
        serverSocket.close();
        serverThread.join();
    }

}
