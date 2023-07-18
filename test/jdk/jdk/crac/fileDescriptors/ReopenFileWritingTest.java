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

import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
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

    @CracTestArg(value = 4, optional = true)
    String fileUseChannel;


    @Override
    public void test() throws Exception {
        Path noAppendPath = Files.createTempFile(getClass().getName(), ".txt");
        Path appendPath = Files.createTempFile(getClass().getName(), ".txt");
        Path appendExtendedPath = Files.createTempFile(getClass().getName(), ".txt");
        Path appendTruncatedPath = Files.createTempFile(getClass().getName(), ".txt");
        Path useChannelPath = Files.createTempFile(getClass().getName(), ".txt");
        fileNoAppend = noAppendPath.toString();
        fileAppend = appendPath.toString();
        fileAppendExtended = appendExtendedPath.toString();
        fileAppendTruncated = appendTruncatedPath.toString();
        fileUseChannel = useChannelPath.toString();
        Path config = writeConfig("""
                type: FILE
                action: reopen
                """);
        try {
            CracBuilder builder = new CracBuilder();
            builder
                    .javaOption(OpenResourcePolicies.PROPERTY, config.toString())
                    .args(CracTest.args(fileNoAppend, fileAppend, fileAppendExtended, fileAppendTruncated, fileUseChannel));
            builder.doCheckpoint();
            assertEquals("Hello ", Files.readString(noAppendPath));
            assertEquals("Hello ", Files.readString(appendPath));
            assertEquals("Hello ", Files.readString(appendExtendedPath));
            assertEquals("Hello ", Files.readString(appendTruncatedPath));
            assertEquals("Hello ", Files.readString(useChannelPath));
            Files.writeString(noAppendPath, "1234567890");
            Files.writeString(appendPath, "123456");
            Files.writeString(appendExtendedPath, "1234567890");
            Files.writeString(appendTruncatedPath, "");
            Files.writeString(useChannelPath, "123456");
            builder.doRestore();
            assertEquals("123456world!", Files.readString(noAppendPath));
            assertEquals("123456world!", Files.readString(appendPath));
            assertEquals("1234567890world!", Files.readString(appendExtendedPath));
            assertEquals("world!", Files.readString(appendTruncatedPath));
            assertEquals("123world!", Files.readString(useChannelPath));
        } finally {
            Files.deleteIfExists(noAppendPath);
            Files.deleteIfExists(appendPath);
            Files.deleteIfExists(appendExtendedPath);
            Files.deleteIfExists(appendTruncatedPath);
            Files.deleteIfExists(useChannelPath);
            Files.deleteIfExists(config);
        }
    }

    @Override
    public void exec() throws Exception {
        try (var w1 = new FileWriter(fileNoAppend);
             var w2 = new FileWriter(fileAppend, true);
             var w3 = new FileWriter(fileAppendExtended, true);
             var w4 = new FileWriter(fileAppendTruncated, true);
             var fos5 = new FileOutputStream(fileUseChannel)) {
            Stream.of(w1, w2, w3, w4).forEach(w -> {
                try {
                    w.write("Hello ");
                    w.flush();
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });
            FileChannel ch5 = fos5.getChannel();
            ch5.write(ByteBuffer.wrap("Hello ".getBytes(StandardCharsets.UTF_8)));
            Core.checkpointRestore();
            Stream.of(w1, w2, w3, w4).forEach(w -> {
                try {
                    w.write("world!");
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });
            ch5.position(3);
            ch5.write(ByteBuffer.wrap("world!".getBytes(StandardCharsets.UTF_8)));
        }
    }
}
