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
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.CountDownLatch;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "linux")
 * @build FDPolicyTestBase
 * @build ReopenNamedFifoTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenNamedFifoTest extends FDPolicyTestBase implements CracTest {
    @CracTestArg(optional = true)
    String fifo;

    @Override
    public void test() throws Exception {
        Path tempDirectory = Files.createTempDirectory(getClass().getName());
        Path pipePath = tempDirectory.resolve("pipe");
        fifo = pipePath.toString();
        assertEquals(0, new ProcessBuilder().inheritIO().command("mkfifo", fifo).start().waitFor());
        // From Java POV this has a path, therefore is treated as a file and not as a named pipe
        Path config = writeConfig("""
                type: file
                action: reopen
                """);
        CracBuilder builder = new CracBuilder()
                .javaOption(OpenResourcePolicies.PROPERTY, config.toString())
                .args(CracTest.args(fifo));
        CracProcess cp = builder.startCheckpoint();

        try (var writer = new FileWriter(fifo)) {
            writer.write("Hello ");
            writer.flush();
            cp.waitForCheckpointed();
            CracProcess rp = builder.captureOutput(true).startRestore();
            CountDownLatch latch = new CountDownLatch(1);
            rp.watch(output -> {
                if (output.contains("RESTORED")) {
                    latch.countDown();
                }
            }, error -> {
                System.err.println(error);
                latch.countDown();
            });
            latch.await();
            writer.write("world!");
            writer.flush();
            rp.waitForSuccess();
        } finally {
            Files.deleteIfExists(pipePath);
            Files.deleteIfExists(tempDirectory);
            Files.deleteIfExists(config);
        }
    }

    @Override
    public void exec() throws Exception {
        try (var reader = new FileReader(fifo)) {
            char[] buf = new char[6];
            assertEquals(buf.length, reader.read(buf));
            assertEquals("Hello ", new String(buf));
            Core.checkpointRestore();
            System.out.println("RESTORED");
            assertEquals(buf.length, reader.read(buf));
            assertEquals("world!", new String(buf));
        }
    }
}
