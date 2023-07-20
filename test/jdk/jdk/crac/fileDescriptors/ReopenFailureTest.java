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
import jdk.internal.crac.OpenResourcePolicies;
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
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "linux")
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
        Path config = writeConfig("""
                type: file
                action: reopen
                """);
        Path path1 = Files.createTempFile(getClass().getName(), ".txt");
        Path path2 = Files.createTempFile(getClass().getName(), ".txt");
        log1 = path1.toString();
        log2 = path2.toString();
        try {
            CracBuilder builder = new CracBuilder()
                    .javaOption(OpenResourcePolicies.PROPERTY, config.toString())
                    .args(CracTest.args(log1, log2));
            builder.doCheckpoint();
            Files.delete(path1);
            Files.setPosixFilePermissions(path2, Collections.emptySet());
            builder.doRestore();
        } finally {
            Files.deleteIfExists(path1);
            Files.deleteIfExists(path2);
            Files.deleteIfExists(config);
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

