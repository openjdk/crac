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
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.FileWriter;
import java.nio.file.Files;
import java.nio.file.Path;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.crac.impl:+open
 * @build ReopenAppendingTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenAppendingTest implements CracTest {
    @CracTestArg(optional = true)
    String tempFile;

    @Override
    public void test() throws Exception {
        tempFile = Files.createTempFile(ReopenAppendingTest.class.getName(), ".txt").toString();
        Path tempPath = Path.of(tempFile);
        try {
            String checkpointPolicies = tempFile + '=' + OpenFDPolicies.BeforeCheckpoint.CLOSE;
            CracBuilder builder = new CracBuilder();
            builder
                    .javaOption(OpenFDPolicies.CHECKPOINT_PROPERTY, checkpointPolicies)
                    .args(CracTest.args(tempFile));
            builder.doCheckpoint();
            assertEquals("Hello ", Files.readString(tempPath));
            builder.doRestore();
            assertEquals("Hello world!", Files.readString(tempPath));
        } finally {
            Files.deleteIfExists(tempPath);
        }
    }

    @Override
    public void exec() throws Exception {
        try (var writer = new FileWriter(tempFile)) {
            writer.write("Hello ");
            writer.flush();
            Core.checkpointRestore();
            writer.write("world!");
        }
    }
}
