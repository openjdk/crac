/*
 * Copyright (c) 2024, Azul Systems, Inc. All rights reserved.
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

import java.nio.ByteBuffer;
import java.nio.file.Path;
import java.nio.file.Paths;
import jdk.crac.Core;

import static jdk.test.lib.Asserts.assertTrue;

import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import jdk.internal.jimage.BasicImageReader;
import jdk.internal.jimage.ImageLocation;

/**
 * @test
 * @summary Tests checkpoint/restore for BasicImageReader with an open descriptor.
 * @library /test/lib
 * @modules java.base/jdk.internal.jimage
 * @build BasicImageReaderTest
 * @run driver jdk.test.lib.crac.CracTest true true
 * @run driver jdk.test.lib.crac.CracTest true false
 * @run driver jdk.test.lib.crac.CracTest false true
 * @run driver jdk.test.lib.crac.CracTest false false
 */
public class BasicImageReaderTest implements CracTest {
    @CracTestArg(0)
    boolean useJvmMap;

    @CracTestArg(1)
    boolean imageMapAll;

    @Override
    public void test() throws Exception {
        CracProcess cp = new CracBuilder().engine(CracEngine.SIMULATE).captureOutput(true)
            .vmOption("--add-opens=java.base/jdk.internal.jimage=ALL-UNNAMED")
            .startCheckpoint();
        cp.outputAnalyzer()
            .shouldHaveExitValue(0);
    }

    @Override
    public void exec() throws Exception {
        System.setProperty("jdk.image.use.jvm.map", String.valueOf(useJvmMap));
        System.setProperty("jdk.image.map.all", String.valueOf(imageMapAll));

        Path imageFile = Paths.get(System.getProperty("java.home"), "lib", "modules");
        BasicImageReader reader = BasicImageReader.open(imageFile);

        readClass(reader, "java.base", "java/lang/String.class");
        readClass(reader, "java.logging", "java/util/logging/Logger.class");

        Core.checkpointRestore();
        System.out.println("RESTORED");

        readClass(reader, "java.base", "java/lang/String.class");
        readClass(reader, "java.logging", "java/util/logging/Logger.class");
    }

    private void readClass(BasicImageReader reader, String moduleName, String className) throws Exception {
        final int classMagic = 0xCAFEBABE;

        System.out.printf("reading: module: %s, path: %s%n", moduleName, className);
        ImageLocation location = reader.findLocation(moduleName, className);
        assertTrue(location != null);

        long size = location.getUncompressedSize();
        assertTrue(size > 0);

        ByteBuffer buffer = reader.getResourceBuffer(location);
        assertTrue(buffer != null);

        final int magic = buffer.getInt();
        assertTrue(magic == classMagic);
    }
}
