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
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.FileReader;
import java.io.FileWriter;
import java.nio.file.Files;
import java.nio.file.Path;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @build FDPolicyTestBase
 * @build ReopenFileReadingTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenFileReadingTest extends FDPolicyTestBase implements CracTest {
    @CracTestArg(optional = true)
    String tempFile;

    @Override
    public void test() throws Exception {
        tempFile = Files.createTempFile(getClass().getName(), ".txt").toString();
        Path configFile = writeConfig("""
                # These first two rules are just to test parsing
                type: FILE
                path: /some/other/file                
                action: error
                ---
                type: FILE
                path: **/*.globpattern.test                
                action: CLOSE
                ---
                type: FILE
                path: $tempFile
                # action is case-insensitive
                action: ReOpeN
                ---
                """.replace("$tempFile", tempFile));
        Path tempPath = Path.of(tempFile);
        try {
            writeBigFile(tempPath, "Hello ", "world!");
            new CracBuilder()
                    .javaOption(OpenResourcePolicies.PROPERTY, configFile.toString())
                    .args(CracTest.args(tempFile)).doCheckpointAndRestore();
        } finally {
            Files.deleteIfExists(tempPath);
            Files.deleteIfExists(configFile);
        }
    }

    @Override
    public void exec() throws Exception {
        try (var reader = new FileReader(tempFile)) {
            char[] buf = new char[6];
            assertEquals(buf.length, reader.read(buf));
            assertEquals("Hello ", new String(buf));
            Core.checkpointRestore();
            readContents(reader);
            assertEquals(buf.length, reader.read(buf));
            assertEquals("world!", new String(buf));
        }
    }
}
