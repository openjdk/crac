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
import jdk.crac.impl.OpenFilePolicies;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertGreaterThan;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.crac.impl:+open
 * @requires (os.family == "linux")
 * @build CloseProcessPipeTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class CloseProcessPipeTest implements CracTest {
    @Override
    public void test() throws Exception {
        String checkpointPolicies = "FIFO=" + OpenFilePolicies.BeforeCheckpoint.CLOSE;
        String restorePolicies = "FIFO=" + OpenFilePolicies.AfterRestore.OPEN_OTHER + "=/dev/null";
        CracBuilder builder = new CracBuilder()
                .javaOption(OpenFilePolicies.CHECKPOINT_PROPERTY, checkpointPolicies)
                .javaOption(OpenFilePolicies.RESTORE_PROPERTY, restorePolicies);
        builder.doCheckpointAndRestore();
    }

    @Override
    public void exec() throws Exception {
        Process process = new ProcessBuilder().command("cat", "/dev/zero").start();
        byte[] buffer = new byte[1024];
        int read1 = process.getInputStream().read(buffer);
        assertGreaterThan(read1, 0);
        Core.checkpointRestore();
        int read2, total = read1;
        // Some data got buffered from /dev/zero, we will still read those.
        // Had we used KEEP_CLOSED policy the read would return IOException: Stream Closed
        // in native code when we try to read from FD -1.
        while ((read2 = process.getInputStream().read(buffer)) >= 0) {
            total += read2;
        }
        System.err.printf("Read total %d bytes%n", total);
        // The process will end with SIGPIPE
        assertEquals(141, process.waitFor());
    }
}
