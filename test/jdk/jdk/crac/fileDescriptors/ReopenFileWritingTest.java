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
import jdk.test.lib.crac.CracTestArg;

import java.io.FileWriter;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.stream.Stream;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.crac.impl:+open
 * @build ReopenFileWritingTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenFileWritingTest implements CracTest {
    @CracTestArg(value = 0, optional = true)
    String fileNoAppend;

    @CracTestArg(value = 1, optional = true)
    String fileAppend;

    @CracTestArg(value = 2, optional = true)
    String fileAppendExtended;

    @CracTestArg(value = 3, optional = true)
    String fileAppendTruncated;

    @Override
    public void test() throws Exception {
        fileNoAppend = Files.createTempFile(ReopenFileWritingTest.class.getName(), ".txt").toString();
        fileAppend = Files.createTempFile(ReopenFileWritingTest.class.getName(), ".txt").toString();
        fileAppendExtended = Files.createTempFile(ReopenFileWritingTest.class.getName(), ".txt").toString();
        fileAppendTruncated = Files.createTempFile(ReopenFileWritingTest.class.getName(), ".txt").toString();
        Path noAppendPath = Path.of(fileNoAppend);
        Path appendPath = Path.of(fileAppend);
        Path appendExtendedPath = Path.of(fileAppendExtended);
        Path appendTruncatedPath = Path.of(fileAppendTruncated);
        try {
            String checkpointPolicies = noAppendPath.getParent().resolve("*").toString() + '=' + OpenFilePolicies.BeforeCheckpoint.CLOSE;
            CracBuilder builder = new CracBuilder();
            builder
                    .javaOption(OpenFilePolicies.CHECKPOINT_PROPERTY, checkpointPolicies)
                    .args(CracTest.args(fileNoAppend, fileAppend, fileAppendExtended, fileAppendTruncated));
            builder.doCheckpoint();
            assertEquals("Hello ", Files.readString(noAppendPath));
            assertEquals("Hello ", Files.readString(appendPath));
            assertEquals("Hello ", Files.readString(appendExtendedPath));
            assertEquals("Hello ", Files.readString(appendTruncatedPath));
            Files.writeString(noAppendPath, "1234567890");
            Files.writeString(appendPath, "123456");
            Files.writeString(appendExtendedPath, "1234567890");
            Files.writeString(appendTruncatedPath, "");
            builder.doRestore();
            assertEquals("123456world!", Files.readString(noAppendPath));
            assertEquals("123456world!", Files.readString(appendPath));
            assertEquals("1234567890world!", Files.readString(appendExtendedPath));
            assertEquals("world!", Files.readString(appendTruncatedPath));
        } finally {
            Files.deleteIfExists(noAppendPath);
            Files.deleteIfExists(appendPath);
            Files.deleteIfExists(appendExtendedPath);
            Files.deleteIfExists(appendTruncatedPath);
        }
    }

    @Override
    public void exec() throws Exception {
        try (var w1 = new FileWriter(fileNoAppend);
             var w2 = new FileWriter(fileAppend, true);
             var w3 = new FileWriter(fileAppendExtended, true);
             var w4 = new FileWriter(fileAppendTruncated, true)) {
            Stream.of(w1, w2, w3, w4).forEach(w -> {
                try {
                    w.write("Hello ");
                    w.flush();
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });
            Core.checkpointRestore();
            Stream.of(w1, w2, w3, w4).forEach(w -> {
                try {
                    w.write("world!");
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });
        }
    }
}
