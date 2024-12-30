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
import jdk.test.lib.Utils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.Arrays;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @build ClasspathParseTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ClasspathParseTest implements CracTest {
    public static final String JAVA = Utils.TEST_JDK + "/bin/java";

    @Override
    public void test() throws Exception {
        String someJar = Arrays.stream(System.getProperty("java.class.path").split(File.pathSeparator))
                .filter(f -> f.endsWith(".jar")).findAny()
                .orElseThrow(() -> new AssertionError("there should be some jar on classpath"));
        new CracBuilder()
                .engine(CracEngine.SIMULATE)
                .printResources(true)
                .classpathEntry(ClasspathParseTest.class.getProtectionDomain().getCodeSource().getLocation().getPath())
                .classpathEntry("file:/C:\\some\\invalid/path")
                .classpathEntry(someJar)
                .startCheckpoint().waitForSuccess();
    }

    @Override
    public void exec() throws Exception {
        // assert that code source path started with "/" as we expect (even on Windows)
        String cp = System.getProperty("java.class.path");
        if (!cp.startsWith("/")) {
            System.exit(2);
        }
        String someJar = Arrays.stream(cp.split(File.pathSeparator)).filter(f -> f.endsWith(".jar")).findAny()
                .orElseThrow(() -> new AssertionError("jar file should be provided on classpath"));
        try (var fis = new FileInputStream(someJar)) {
            Core.checkpointRestore();
        }
    }
}
