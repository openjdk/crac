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
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.crac.impl:+open
 * @build FDPolicyTestBase
 * @build ReopenAtEndTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenAtEndTest extends FDPolicyTestBase implements CracTest {
    @CracTestArg(value = 0, optional = true)
    String log1;

    @CracTestArg(value = 1, optional = true)
    String log2;

    @CracTestArg(value = 2, optional = true)
    String log3;

    @Override
    public void test() throws Exception {
        log1 = Files.createTempFile(ReopenAtEndTest.class.getName(), ".txt").toString();
        log2 = Files.createTempFile(ReopenAtEndTest.class.getName(), ".txt").toString();
        log3 = Files.createTempFile(ReopenAtEndTest.class.getName(), ".txt").toString();
        try {
            Files.writeString(Path.of(log3), "333");
            String checkpointPolicies = "/**/*=" + OpenFDPolicies.BeforeCheckpoint.CLOSE;
            String restorePolicies = log1 + '=' + OpenFDPolicies.AfterRestore.REOPEN_AT_END + File.pathSeparatorChar +
                    log2 + '=' + OpenFDPolicies.AfterRestore.OPEN_OTHER_AT_END + '=' + log3;
            CracBuilder builder = new CracBuilder()
                    .javaOption(OpenFDPolicies.CHECKPOINT_PROPERTY, checkpointPolicies)
                    .javaOption(OpenFDPolicies.RESTORE_PROPERTY, restorePolicies)
                    .args(CracTest.args(log1, log2, log3));
            builder.doCheckpoint();
            Files.writeString(Path.of(log1), "ZZZ", StandardOpenOption.APPEND);
            builder.doRestore();
            assertEquals("1ZZZX", Files.readString(Path.of(log1)));
            assertEquals("22", Files.readString(Path.of(log2)));
            assertEquals("333Y", Files.readString(Path.of(log3)));
        } finally {
            Files.deleteIfExists(Path.of(log1));
            Files.deleteIfExists(Path.of(log2));
            Files.deleteIfExists(Path.of(log3));
        }
    }

    @Override
    public void exec() throws Exception {
        try (var reader1 = new FileWriter(log1); var reader2 = new FileWriter(log2)) {
            reader1.write("1");
            reader1.flush();
            reader2.write("22");
            reader2.flush();
            Core.checkpointRestore();
            reader1.write("X");
            reader2.write("Y");
        }
    }
}

