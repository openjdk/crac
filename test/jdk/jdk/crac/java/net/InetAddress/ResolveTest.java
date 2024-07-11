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

import jdk.crac.*;
import jdk.test.lib.Utils;
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.concurrent.*;

/*
 * @test
 * @summary Test if InetAddress cache is flushed after checkpoint/restore
 * @requires (os.family == "linux")
 * @requires docker.support
 * @library /test/lib
 * @build ResolveTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ResolveTest implements CracTest {
    public static final String TEST_HOSTNAME = "some.test.hostname.example.com";

    @CracTestArg(value = 0, optional = true)
    String ip;

    // A file mounted on restore was used to identify a restore but now it is
    // not possible to check for file existance until restoration completes (the
    // check would use native buffers which are restorable resources themselves)
    Resource restoreListener;

    @Override
    public void test() throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            return;
        }

        String imageName = Common.imageName("inet-address");

        CracBuilder builder = new CracBuilder()
                .inDockerImage(imageName).dockerOptions("--add-host", TEST_HOSTNAME + ":192.168.12.34")
                .captureOutput(true)
                .args(CracTest.args(TEST_HOSTNAME));

        try {
            CompletableFuture<?> firstOutputFuture = new CompletableFuture<Void>();
            builder.vmOption("-XX:CRaCMinPid=100");
            CracProcess checkpointed = builder.startCheckpoint().watch(line -> {
                System.out.println("OUTPUT: " + line);
                if (line.equals("192.168.12.34")) {
                    firstOutputFuture.complete(null);
                }
            }, error -> {
                System.err.println("ERROR: " + error);
                firstOutputFuture.cancel(false);
            });
            firstOutputFuture.get(10, TimeUnit.SECONDS);
            builder.checkpointViaJcmd();
            checkpointed.waitForCheckpointed();

            builder.clearVmOptions();
            builder.recreateContainer(imageName,
                    "--add-host", TEST_HOSTNAME + ":192.168.56.78");

            builder.startRestore().outputAnalyzer()
                    .shouldHaveExitValue(0)
                    .shouldContain("192.168.56.78");
        } finally {
            builder.ensureContainerKilled();
        }
    }

    @Override
    public void exec() throws Exception {
        if (ip == null) {
            System.err.println("Args: <ip address>");
            return;
        }

        restoreListener = new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) {
            }
            @Override
            public void afterRestore(Context<? extends Resource> context) {
                restoreListener = null;
            }
        };
        Core.getGlobalContext().register(restoreListener);

        printAddress(ip);
        while (restoreListener != null) { // While not restored
            try {
                //noinspection BusyWait
                Thread.sleep(100);
            } catch (InterruptedException e) {
                System.err.println("Interrupted!");
                return;
            }
        }
        printAddress(ip);
    }

    private static void printAddress(String hostname) {
        try {
            InetAddress address = InetAddress.getByName(hostname);
            // we will assume IPv4 address
            byte[] bytes = address.getAddress();
            System.out.print(bytes[0] & 0xFF);
            for (int i = 1; i < bytes.length; ++i) {
                System.out.print('.');
                System.out.print(bytes[i] & 0xFF);
            }
            System.out.println();
        } catch (UnknownHostException e) {
            System.out.println();
        }
    }
}
