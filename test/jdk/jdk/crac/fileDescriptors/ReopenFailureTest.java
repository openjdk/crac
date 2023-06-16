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
import jdk.crac.RestoreException;
import jdk.crac.impl.OpenFilePolicies;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.FileWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Collections;

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.fail;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.crac.impl:+open
 * @build FDPolicyTestBase
 * @build ReopenFailureTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenFailureTest extends FDPolicyTestBase implements CracTest {
    @CracTestArg(value = 0, optional = true)
    String log1;

    @CracTestArg(value = 1, optional = true)
    String log2;

    @Override
    public void test() throws Exception {
        log1 = Files.createTempFile(ReopenFailureTest.class.getName(), ".txt").toString();
        log2 = Files.createTempFile(ReopenFailureTest.class.getName(), ".txt").toString();
        try {
            String checkpointPolicies = "/**/*=" + OpenFilePolicies.BeforeCheckpoint.CLOSE;
            CracBuilder builder = new CracBuilder()
                    .javaOption(OpenFilePolicies.CHECKPOINT_PROPERTY, checkpointPolicies)
                    .args(CracTest.args(log1, log2));
            builder.doCheckpoint();
            Files.delete(Path.of(log1));
            Files.setPosixFilePermissions(Path.of(log2), Collections.emptySet());
            builder.doRestore();
        } finally {
            Files.deleteIfExists(Path.of(log1));
            Files.deleteIfExists(Path.of(log2));
        }
    }

    @Override
    public void exec() throws Exception {
        try (var writer1 = new FileWriter(log1);
             var writer2 = new FileWriter(log2, true)) {
            writer1.write("Hello!");
            writer1.flush();
            writer2.write("Hello!");
            writer2.flush();
            try {
                Core.checkpointRestore();
                fail("Should throw");
            } catch (RestoreException ex) {
                assertEquals(2, ex.getSuppressed().length);
            }
        }
    }
}

