/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
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

/*
 * @test
 * @summary Unit test for ProcessTools.executeLimitedTestJava()
 * @library /test/lib
 * @run main/othervm -Dtest.java.opts=-XX:MaxMetaspaceSize=123456789 ProcessToolsExecuteLimitedTestJavaTest
 */

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class ProcessToolsExecuteLimitedTestJavaTest {
    public static void main(String[] args) throws Exception {
        if (args.length > 0) {
            // Do nothing. Just let the JVM log its output.
        } else {
            // In comparison to executeTestJava, executeLimitedTestJava should not add the
            // -Dtest.java.opts flags. Check that it doesn't.
            OutputAnalyzer output = ProcessTools.executeLimitedTestJava("-XX:+PrintFlagsFinal", "-version");
            output.stdoutShouldNotMatch(".*MaxMetaspaceSize.* = 123456789.*");
        }
    }
}
