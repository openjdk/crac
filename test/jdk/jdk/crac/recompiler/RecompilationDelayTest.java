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
 * @summary Recompilation should not happen earlier than after the specified
 *          delay after the restoring finishes.
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar wb.jar jdk.test.whitebox.WhiteBox
 * @build RecompilationDelayTest
 * @run driver jdk.test.lib.crac.CracTest -1
 * @run driver jdk.test.lib.crac.CracTest 0
 * @run driver jdk.test.lib.crac.CracTest 10000
 */
public class RecompilationDelayTest implements CracTest {
    @CracTestArg(0)
    long delayMs;

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
        """.formatted(RecompilationDelayTest.class.getName().replace('.', '/'), TEST_METHOD_NAME);
    private static final int TEST_METHOD_COMP_LEVEL = 4;

    @Override
    public void test() throws Exception {
        new CracBuilder().engine(CracEngine.SIMULATE)
            .vmOption("-Xbootclasspath/a:wb.jar").vmOption("-XX:+UnlockDiagnosticVMOptions").vmOption("-XX:+WhiteBoxAPI")
            .javaOption("jdk.crac.recompilation-delay-ms", Long.toString(delayMs))
            .startCheckpoint().waitForSuccess();
    }

    @Override
    public void exec() throws Exception {
        final var whiteBox = WhiteBox.getWhiteBox();
        final var testMethodRef = RecompilationDelayTest.class.getDeclaredMethod(TEST_METHOD_NAME);

        assertEquals(
            1, whiteBox.addCompilerDirective(MAKE_TEST_METHOD_COMP_BLOCKING_DIRECTIVE),
            "Unexpected number of directives installed"
        );

        assertTrue(
            whiteBox.enqueueMethodForCompilation(testMethodRef, TEST_METHOD_COMP_LEVEL),
            "Failed to compile immediately"
        );
        assertEquals(
            TEST_METHOD_COMP_LEVEL, whiteBox.getMethodCompilationLevel(testMethodRef),
            "Unexpected pre-C/R compilation level"
        );

        final var resource = new Resource() {
            public long restoreFinishTimeMs = -1;

            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {}

            @Override
            public void afterRestore(Context<? extends Resource> context) throws Exception {
                if (delayMs <= 0) {
                    assertEquals(
                        1, whiteBox.deoptimizeMethod(testMethodRef),
                        "Unexpected number of deoptimizations"
                    );
                } else {
                    restoreFinishTimeMs = Math.floorDiv(System.nanoTime(), 1_000_000);
                }
            }
        };
        Core.getGlobalContext().register(resource);

        Core.checkpointRestore();

        if (delayMs > 0) {
            assertEquals(
                1, whiteBox.deoptimizeMethod(testMethodRef),
                "Unexpected number of deoptimizations"
            );

            // Ensure the delay has not expired before we triggered the decompilation
            final var timeSinceRestoreFinishMs = Math.ceilDiv(System.nanoTime(), 1_000_000) - resource.restoreFinishTimeMs;
            assertLessThan(timeSinceRestoreFinishMs, delayMs, "Specified delay is too low for this machine");

            // Wait for the delay to expire and give the concurrent compilations some time to finish
            Thread.sleep(delayMs + 500);
        }

        assertEquals(
            TEST_METHOD_COMP_LEVEL, whiteBox.getMethodCompilationLevel(testMethodRef),
            "Unexpected post-C/R compilation level"
        );
    }
}
