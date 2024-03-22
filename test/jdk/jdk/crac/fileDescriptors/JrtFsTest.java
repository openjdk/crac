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

import java.net.URI;
import java.nio.file.Files;
import java.nio.file.FileSystem;
import java.nio.file.FileSystems;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.stream.Stream;
import jdk.crac.Core;

import static jdk.test.lib.Asserts.assertTrue;

import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

/**
 * @test
 * @summary Tests creating JRT FS and checkpoint'ing with an open file descriptor.
 * @library /test/lib
 * @build JrtFsTest
 * @run driver jdk.test.lib.crac.CracTest true true
 * @run driver jdk.test.lib.crac.CracTest true false
 * @run driver jdk.test.lib.crac.CracTest false true
 * @run driver jdk.test.lib.crac.CracTest false false
 */
public class JrtFsTest implements CracTest {
    @CracTestArg(0)
    boolean useJvmMap;

    @CracTestArg(1)
    boolean imageMapAll;

    @Override
    public void test() throws Exception {
        CracProcess cp = new CracBuilder().engine(CracEngine.SIMULATE).captureOutput(true)
            .startCheckpoint();
        cp.outputAnalyzer()
            .shouldHaveExitValue(0);
    }

    @Override
    public void exec() throws Exception {
        System.setProperty("jdk.image.use.jvm.map", String.valueOf(useJvmMap));
        System.setProperty("jdk.image.map.all", String.valueOf(imageMapAll));
        Map<String, String> env = new HashMap<>();
        env.put("java.home", System.getProperty("java.home"));
        FileSystem fs = FileSystems.newFileSystem(URI.create("jrt:/"), env);

        byte[] classBytes1;
        {
            Path objectClassPath = fs.getPath("/modules/java.base", "java/lang/Object.class");
            classBytes1 = Files.readAllBytes(objectClassPath);
            System.out.println("Read " + classBytes1.length + " bytes from Object.class");
        }

        Core.checkpointRestore();
        System.out.println("RESTORED");

        byte[] classBytes2;
        {
            Path objectClassPath = fs.getPath("/modules/java.base", "java/lang/Object.class");
            classBytes2 = Files.readAllBytes(objectClassPath);
            System.out.println("Read " + classBytes2.length + " bytes from Object.class");
        }
        assertTrue(Arrays.equals(classBytes1, classBytes2));
    }
}
