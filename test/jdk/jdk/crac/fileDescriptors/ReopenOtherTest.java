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
import jdk.crac.impl.OpenFilePolicies;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.FileReader;
import java.nio.file.Files;
import java.nio.file.Path;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.crac.impl:+open
 * @build FDPolicyTestBase
 * @build ReopenOtherTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenOtherTest extends FDPolicyTestBase implements CracTest {
    @CracTestArg(value = 0, optional = true)
    String helloWorld;

    @CracTestArg(value = 1, optional = true)
    String nazdarSvete;

    @Override
    public void test() throws Exception {
        helloWorld = Files.createTempFile(ReopenOtherTest.class.getName(), ".txt").toString();
        nazdarSvete = Files.createTempFile(ReopenOtherTest.class.getName(), ".txt").toString();
        Path hwPath = Path.of(helloWorld);
        Path nsPath = Path.of(nazdarSvete);
        try {
            writeBigFile(hwPath, "Hello ", "world!");
            writeBigFile(nsPath, "Nazdar", "svete!");
            String checkpointPolicies = helloWorld + '=' + OpenFilePolicies.BeforeCheckpoint.WARN_CLOSE;
            String restorePolicies = helloWorld + '=' + OpenFilePolicies.AfterRestore.OPEN_OTHER + '=' + nazdarSvete;
            CracBuilder builder = new CracBuilder()
                    .captureOutput(true)
                    .javaOption(OpenFilePolicies.CHECKPOINT_PROPERTY, checkpointPolicies)
                    .javaOption(OpenFilePolicies.RESTORE_PROPERTY, restorePolicies)
                    .args(CracTest.args(helloWorld));
            CracProcess cp = builder.startCheckpoint();
            cp.waitForCheckpointed();
            cp.outputAnalyzer().stderrShouldContain("was not closed by the application");
            builder.captureOutput(false).doRestore();
        } finally {
            Files.deleteIfExists(hwPath);
            Files.deleteIfExists(nsPath);
        }
    }

    @Override
    public void exec() throws Exception {
        try (var reader = new FileReader(helloWorld)) {
            char[] buf = new char[6];
            assertEquals(buf.length, reader.read(buf));
            assertEquals("Hello ", new String(buf));
            Core.checkpointRestore();
            readContents(reader);
            assertEquals(buf.length, reader.read(buf));
            assertEquals("svete!", new String(buf));
        }
    }
}
