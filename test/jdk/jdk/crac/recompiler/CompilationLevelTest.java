/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.Context;
import jdk.crac.Core;
import jdk.crac.Resource;
import static jdk.test.lib.Asserts.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;
import jdk.test.whitebox.WhiteBox;

/*
 * @test
 * @summary Should recompile iff the recorded compilation level was better than
 *          the current one.
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar wb.jar jdk.test.whitebox.WhiteBox
 * @build CompilationLevelTest
 * @run driver jdk.test.lib.crac.CracTest 4 0 true
 * @run driver jdk.test.lib.crac.CracTest 4 3 true
 * @run driver jdk.test.lib.crac.CracTest 2 0 true
 * @run driver jdk.test.lib.crac.CracTest 1 4 false
 * @run driver jdk.test.lib.crac.CracTest 4 1 false
 * @run driver jdk.test.lib.crac.CracTest 3 2 false
 * @run driver jdk.test.lib.crac.CracTest 0 3 false
 */
public class CompilationLevelTest implements CracTest {
    @CracTestArg(0)
    int preCrCompilationLevel;
    @CracTestArg(1)
    int inCrCompilationLevel;
    @CracTestArg(2)
    boolean shouldRecompile;

    @SuppressWarnings("unused")
    private static void testMethod() {}

    private static final String TEST_METHOD_NAME = "testMethod";
    private static final String MAKE_TEST_METHOD_COMP_BLOCKING_DIRECTIVE =
        """
        [
            {
                match: "%s.%s",
                BackgroundCompilation: false
            }
        ]
        """.formatted(CompilationLevelTest.class.getName().replace('.', '/'), TEST_METHOD_NAME);

    @Override
    public void test() throws Exception {
        new CracBuilder().engine(CracEngine.SIMULATE)
            .vmOption("-Xbootclasspath/a:wb.jar").vmOption("-XX:+UnlockDiagnosticVMOptions").vmOption("-XX:+WhiteBoxAPI")
            .javaOption("jdk.crac.recompilation-delay-ms", "0")
            .startCheckpoint().waitForSuccess();
    }

    @Override
    public void exec() throws Exception {
        final var whiteBox = WhiteBox.getWhiteBox();
        final var testMethodRef = CompilationLevelTest.class.getDeclaredMethod(TEST_METHOD_NAME);

        assertEquals(
            1, whiteBox.addCompilerDirective(MAKE_TEST_METHOD_COMP_BLOCKING_DIRECTIVE),
            "Unexpected number of directives installed"
        );

        if (preCrCompilationLevel > 0) {
            assertTrue(
                whiteBox.enqueueMethodForCompilation(testMethodRef, preCrCompilationLevel),
                "Failed to compile immediately"
            );
        }
        assertEquals(
            preCrCompilationLevel, whiteBox.getMethodCompilationLevel(testMethodRef),
            "Unexpected pre-C/R compilation level"
        );

        final var resource = new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) {
                if (preCrCompilationLevel > 0) {
                    assertEquals(
                        1, whiteBox.deoptimizeMethod(testMethodRef),
                        "Unexpected number of deoptimizations"
                    );
                }
            }

            @Override
            public void afterRestore(Context<? extends Resource> context) {
                if (inCrCompilationLevel > 0) {
                    assertTrue(
                        whiteBox.enqueueMethodForCompilation(testMethodRef, inCrCompilationLevel),
                        "Failed to compile immediately"
                    );
                }
                assertEquals(
                    inCrCompilationLevel, whiteBox.getMethodCompilationLevel(testMethodRef),
                    "Unexpected in-C/R compilation level"
                );
            }
        };
        Core.getGlobalContext().register(resource);

        Core.checkpointRestore();

        final var expectedCompilationLevel = shouldRecompile ? preCrCompilationLevel : inCrCompilationLevel;
        assertEquals(
            expectedCompilationLevel, whiteBox.getMethodCompilationLevel(testMethodRef),
            "Unexpected post-C/R compilation level"
        );
    }
}
