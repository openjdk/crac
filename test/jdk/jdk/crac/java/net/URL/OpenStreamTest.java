/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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
import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.URL;
import java.nio.file.Files;
import java.nio.file.Path;

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertTrue;

/*
 * @test
 * @library /test/lib
 * @build OpenStreamTest
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest
 */
public class OpenStreamTest implements CracTest {
    public static final String TEST_STRING = "test\n";

    @Override
    public void test() throws Exception {
        // no restore with simengine
        new CracBuilder().engine(CracEngine.SIMULATE).doCheckpointToAnalyze().shouldHaveExitValue(0);
    }

    private static Path createTestJar() throws IOException {
        Path temp = Files.createTempDirectory(OpenStreamTest.class.getName());
        Path testFilePath = temp.resolve("test.txt");
        try {
            Files.writeString(testFilePath, TEST_STRING);
            Path jarfile = Path.of("test.jar");
            jdk.test.lib.util.JarUtils.createJarFile(jarfile, temp, "test.txt");
            return jarfile;
        } finally {
            File testTxt = testFilePath.toFile();
            if (testTxt.exists()) {
                assert testTxt.delete();
            }
            assert temp.toFile().delete();
        }
    }

    @Override
    public void exec() throws Exception {
        Path testjar = createTestJar();

        URL url = new URL("jar:file:test.jar!/test.txt");
        try (InputStream inputStream = url.openStream()) {
            CRaCMXBean.getCRaCMXBean().checkpointRestore();
            assertEquals(TEST_STRING.length(), inputStream.readAllBytes().length);
        } finally {
            assertTrue(testjar.toFile().delete());
        }
    }

}
