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

import jdk.test.lib.Utils;

import java.io.IOException;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @run driver ClasspathParseTest
 */
public class ClasspathParseTest {
    public static final String JAVA = Utils.TEST_JDK + "/bin/java";

    public static void main(String[] args) throws IOException, InterruptedException {
        if (args.length == 0) {
            String classpath = ClasspathParseTest.class.getProtectionDomain().getCodeSource().getLocation().getPath();
            int exit = new ProcessBuilder().command(JAVA, "-cp", classpath, ClasspathParseTest.class.getName(), "ignored")
                    .inheritIO().start().waitFor();
            assertEquals(0, exit);
        } else {
            // assert that code source path started with "/" as we expect (even on Windows)
            if (!System.getProperty("java.class.path").startsWith("/")) {
                System.exit(2);
            }
        }
    }
}
