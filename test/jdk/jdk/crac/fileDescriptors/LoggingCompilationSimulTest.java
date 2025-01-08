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

import jdk.crac.Core;
import jdk.internal.crac.JDKFdResource;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracEngine;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

import static jdk.test.lib.Asserts.*;

/**
 * @test Check the -XX:+LogVMOutput together with -XX:+LogCompilation flag on simulated crac engine
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @build LoggingVMlogOpenSimulTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class LoggingVMlogOpenSimulTest implements CracTest {
    @Override
    public void test() throws Exception {
        Path logPathO = Files.createTempFile(getClass().getName(), "-vmlog1.txt");
        try {
            CracBuilder builder = new CracBuilder().captureOutput(true);
            builder.engine(CracEngine.SIMULATE);
            builder.vmOption("-Xcomp");
            builder.vmOption("-XX:+UnlockDiagnosticVMOptions");
            builder.vmOption("-XX:+LogVMOutput");
            builder.vmOption("-XX:+LogCompilation");
            builder.vmOption("-XX:LogFile=" + logPathO);
            var oa = builder.startCheckpoint().waitForSuccess().outputAnalyzer();
            oa.shouldContain(RESTORED_MESSAGE);
            assertNotEquals(0, Files.size(logPathO));
        } finally {
            Files.deleteIfExists(logPathO);
        }
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
        System.out.println(RESTORED_MESSAGE);
    }
}
