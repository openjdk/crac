/*
 * Copyright (c) 2024, Azul Systems, Inc. All rights reserved.
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

import java.nio.file.Files;
import java.nio.file.Path;

import jdk.crac.Core;
import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertFalse;
import static jdk.test.lib.Asserts.assertNotEquals;
import static jdk.test.lib.Asserts.assertTrue;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

/**
 * @test Checkpoint with -XX:+LogVMOutput and -XX:+LogCompilation
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "linux")
 * @build LoggingCompilationTest
 * @run driver jdk.test.lib.crac.CracTest true  false
 * @run driver jdk.test.lib.crac.CracTest false true
 * @run driver jdk.test.lib.crac.CracTest true  true
 */
public class LoggingCompilationTest implements CracTest {

    @CracTestArg(0)
    boolean vmLogOnCheckpoint;

    @CracTestArg(1)
    boolean vmLogOnRestore;

    @Override
    public void test() throws Exception {
        Path logPathO = Files.createTempFile(getClass().getName(), "-vmlog1.txt");
        Path logPathR = Files.createTempFile(getClass().getName(), "-vmlog2.txt");
        Files.deleteIfExists(logPathR);
        try {
            CracBuilder builder = new CracBuilder();
            if (vmLogOnCheckpoint) {
                builder.vmOption("-XX:+UnlockDiagnosticVMOptions");
                builder.vmOption("-XX:+LogVMOutput");
                builder.vmOption("-Xcomp");
                builder.vmOption("-XX:+LogCompilation");
                builder.vmOption("-XX:LogFile=" + logPathO);
            }
            builder.startCheckpoint().waitForCheckpointed();
            if (vmLogOnCheckpoint) {
                assertNotEquals(0L, Files.size(logPathO));
                Files.deleteIfExists(logPathO);
            } else {
                assertEquals(0L, Files.size(logPathO));
            }
            builder.clearVmOptions();
            if (vmLogOnRestore) {
                builder.vmOption("-XX:+UnlockDiagnosticVMOptions");
                builder.vmOption("-Xcomp");
                builder.vmOption("-XX:+LogVMOutput");
                builder.vmOption("-XX:LogFile=" + logPathR);
            }
            var oa =builder.captureOutput(true).doRestore().outputAnalyzer()
                    .shouldNotContain("CRaC closing file descriptor")
                    .shouldNotContain("Could not flush log")
                    .shouldNotContain("Could not close log file")
                    .shouldNotContain("Bad file descriptor")
                    .shouldContain(RESTORED_MESSAGE);
        } finally {
            if (vmLogOnRestore) {
                assertTrue(Files.exists(logPathR));
            } else {
                assertFalse(Files.exists(logPathO));
                assertFalse(Files.exists(logPathR));
            }
            Files.deleteIfExists(logPathO);
            Files.deleteIfExists(logPathR);
        }
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
        System.out.println(RESTORED_MESSAGE);
    }
}
