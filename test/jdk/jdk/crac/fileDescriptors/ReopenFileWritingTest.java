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

import java.io.FileWriter;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.stream.Stream;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @build FDPolicyTestBase
 * @build ReopenFileWritingTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ReopenFileWritingTest extends FDPolicyTestBase implements CracTest {
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
        Path noAppendPath = Files.createTempFile(getClass().getName(), ".txt");
        Path appendPath = Files.createTempFile(getClass().getName(), ".txt");
        Path appendExtendedPath = Files.createTempFile(getClass().getName(), ".txt");
        Path appendTruncatedPath = Files.createTempFile(getClass().getName(), ".txt");
        fileNoAppend = noAppendPath.toString();
        fileAppend = appendPath.toString();
        fileAppendExtended = appendExtendedPath.toString();
        fileAppendTruncated = appendTruncatedPath.toString();
        Path config = writeConfig("""
                type: FILE
                action: reopen
                """);
        try {
            CracBuilder builder = new CracBuilder();
            builder
                    .javaOption(OpenResourcePolicies.PROPERTY, config.toString())
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
            // Note: current implementation in FileOutputStream does not record FD offset
            //       and always opens the file for appending
            assertEquals("1234567890world!", Files.readString(noAppendPath));
            assertEquals("123456world!", Files.readString(appendPath));
            assertEquals("1234567890world!", Files.readString(appendExtendedPath));
            assertEquals("world!", Files.readString(appendTruncatedPath));
        } finally {
            Files.deleteIfExists(noAppendPath);
            Files.deleteIfExists(appendPath);
            Files.deleteIfExists(appendExtendedPath);
            Files.deleteIfExists(appendTruncatedPath);
            Files.deleteIfExists(config);
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
