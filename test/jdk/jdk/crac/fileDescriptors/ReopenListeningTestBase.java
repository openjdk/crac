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
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;

import java.io.Closeable;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.CompletableFuture;

import static jdk.test.lib.Asserts.assertFalse;
import static jdk.test.lib.Asserts.assertTrue;

public abstract class ReopenListeningTestBase<ServerType extends Closeable> extends FDPolicyTestBase implements CracTest {

    protected interface Acceptor<ServerType> {
        void accept(ServerType serverSocket) throws Exception;
    }

    @Override
    public void test() throws Exception {
        Path config = writeConfig("""
                type: SOCKET
                family: ip
                listening: true
                action: reopen
                """);
        try {
            new CracBuilder()
                    .engine(CracEngine.SIMULATE)
                    .javaOption(OpenResourcePolicies.PROPERTY, config.toString())
                    .javaOption("jdk.crac.collect-fd-stacktraces", "true")
                    .startCheckpoint().waitForSuccess();
        } finally {
            Files.deleteIfExists(config);
        }
    }

    @Override
    public void exec() throws Exception {
        ServerType serverSocket = createServer();
        testConnection(serverSocket);

        CompletableFuture<Boolean> cf = asyncAccept(serverSocket);
        // There's no way to even check that the accepting thread entered accept() or select()
        Thread.sleep(100);
        assertFalse(cf.isDone());

        Core.checkpointRestore();
        connectClient(serverSocket);
        assertFalse(cf.get());

        testConnection(serverSocket);
        serverSocket.close();
    }

    protected abstract ServerType createServer() throws IOException;

    protected abstract boolean acceptClient(ServerType serverType) throws Exception;

    // This method creates the connection and validates that the port is open;
    // it does not wait for the other party to accept the connection.
    protected abstract void connectClient(ServerType serverSocket) throws Exception;

    private void testConnection(ServerType serverSocket) throws Exception {
        CompletableFuture<Boolean> cf = asyncAccept(serverSocket);
        connectClient(serverSocket);
        assertTrue(cf.get());
    }

    protected CompletableFuture<Boolean> asyncAccept(ServerType serverSocket) {
        CompletableFuture<Boolean> cf = new CompletableFuture<>();
        Thread serverThread = new Thread(() -> {
            try {
                cf.complete(acceptClient(serverSocket));
            } catch (Throwable t) {
                cf.completeExceptionally(t);
            }
        });
        serverThread.setDaemon(true);
        serverThread.start();
        return cf;
    }
}
