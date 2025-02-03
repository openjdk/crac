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
 * Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale
 * CA 94089 USA or visit www.azul.com if you need additional information or
 * have any questions.
 */

import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.util.Arrays;

/*
 * @test RestoreNewArgsTest
 * @summary the test checks new args are propagated into a restored process.
 * @library /test/lib
 * @build RestoreNewArgsTest
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest
 * @requires (os.family == "linux")
 */
public class RestoreNewArgsTest implements CracTest {

    @Override
    public void test() throws Exception {
        final String MAIN_CLASS = "RestoreNewArgsTest$InternalMain";
        final String ARG0 = "test arg";
        final String ARG1 = "\\ another\\ \"test\\\\arg ";
        final String ARG2 = "  ano\007ther  'yet  arg  \\";
        CracBuilder builder = new CracBuilder().captureOutput(true);
        builder.doCheckpoint();
        builder.startRestoreWithArgs(null, Arrays.asList(MAIN_CLASS, ARG0, ARG1, ARG2))
            .waitForSuccess().outputAnalyzer()
            .shouldContain("RESTORED")
            .shouldContain("Arg 0: " + ARG0 + ".")
            .shouldContain("Arg 1: " + ARG1 + ".")
            .shouldContain("Arg 2: " + ARG2 + ".");
    }

    @Override
    public void exec() throws Exception {
        jdk.crac.Core.checkpointRestore();
        System.out.println("RESTORED");
    }

    public class InternalMain {
        public static void main(String args[]) throws Exception {
            int i = 0;
            for (var arg : args) {
                System.out.println("Arg " + i++ + ": " + arg + ".");
            }
        }
    }
}

