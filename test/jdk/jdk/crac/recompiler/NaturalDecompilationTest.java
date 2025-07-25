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

import java.util.Objects;
import java.util.function.BooleanSupplier;

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
 * @summary Tests recompilation of a method deoptimized because of reaching an
 *          uncompiled path (e.g. uncommon trap). Other recompiler tests rely on
 *          WhiteBox to force a method to compile/decompile — this test is
 *          supposed to check a more real-life scenerio instead.
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar wb.jar jdk.test.whitebox.WhiteBox
 * @build NaturalDecompilationTest
 * @run driver jdk.test.lib.crac.CracTest TIERED
 * @run driver jdk.test.lib.crac.CracTest C1_ONLY
 * @run driver jdk.test.lib.crac.CracTest C2_ONLY
 */
public class NaturalDecompilationTest implements CracTest {
    private static final long STAGE_TIME_LIMIT_SEC = 30;

    // The compilers have different deoptimization implementations so it is
    // worth to test each of them.
    public enum Variant {
        TIERED,
        C1_ONLY,
        C2_ONLY,
    };

    @CracTestArg
    Variant variant;

    private static void blackhole(@SuppressWarnings("unused") Object o) {}

    private static int testMethod(int i) {
        try {
            // A compiler intrinsic which triggers decompilation in both C1 and
            // C2 when it has to throw IndexOutOfBoundsException
            Objects.checkIndex(i, 10);
            return i;
        } catch (IndexOutOfBoundsException ignored) {
            return -1;
        }
    }

    private static final String BLACKHOLE_NAME = "blackhole";
    private static final String TEST_METHOD_NAME = "testMethod";

    private static final int TEST_ARG_EXPECTED = 0;
    private static final int TEST_ARG_UNEXPECTED = -1;

    @Override
    public void test() throws Exception {
        final var builder = new CracBuilder().engine(CracEngine.SIMULATE).captureOutput(true)
            .vmOption("-Xbootclasspath/a:wb.jar").vmOption("-XX:+UnlockDiagnosticVMOptions").vmOption("-XX:+WhiteBoxAPI")
            .javaOption("jdk.crac.recompilation-delay-ms", "0")
            .vmOption("-XX:+UnlockExperimentalVMOptions")
            .vmOption("-XX:CompileCommand=blackhole," + NaturalDecompilationTest.class.getName() + "." + BLACKHOLE_NAME)
            .vmOption("-XX:CompileCommand=dontinline," + NaturalDecompilationTest.class.getName() + "." + TEST_METHOD_NAME)
            .vmOption("-XX:+PrintCompilation")
            .vmOption("-Xlog:crac=trace");
        switch (variant) {
            case TIERED -> { /* This is the default */ }
            case C1_ONLY -> builder.vmOption("-XX:TieredStopAtLevel=1");
            case C2_ONLY -> builder.vmOption("-XX:-TieredCompilation");
        }

        // Must create an output analyzer before waiting for success, otherwise
        // on Windows PrintCompilation overflows the piping buffer and the
        // waiting never completes
        final var proc = builder.startCheckpoint();
        final var out = proc.outputAnalyzer();
        proc.waitForSuccess();
        out.shouldContain("Requesting recompilation: int " + NaturalDecompilationTest.class.getName() + "." + TEST_METHOD_NAME + "(int)");
    }

    @Override
    public void exec() throws Exception {
        final var whiteBox = WhiteBox.getWhiteBox();
        final var testMethodRef = NaturalDecompilationTest.class.getDeclaredMethod(TEST_METHOD_NAME, int.class);

        timedDoWhile("compilation", () -> {
            for (int i = 0; i < 2000; i++) {
                final var res = testMethod(TEST_ARG_EXPECTED);
                blackhole(res);
            }
            try {
                Thread.sleep(500); // Time to compile
            } catch (InterruptedException ex) {
                throw new RuntimeException(ex);
            }
            return whiteBox.isMethodCompiled(testMethodRef);
        });

        final var resource = new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
                assertTrue(whiteBox.isMethodCompiled(testMethodRef), "Should still be compiled");
                timedDoWhile("deoptimization", () -> {
                    // We don't want to call to many times or the method may
                    // get compiled again. Normally just one call is enough
                    // to make it decompile,
                    testMethod(TEST_ARG_UNEXPECTED);
                    return !whiteBox.isMethodCompiled(testMethodRef);
                });
            }

            @Override
            public void afterRestore(Context<? extends Resource> context) throws Exception {
                assertFalse(whiteBox.isMethodCompiled(testMethodRef), "Should still be deoptimized");
            }
        };
        Core.getGlobalContext().register(resource);

        Core.checkpointRestore();

        timedDoWhile("recompilation", () -> {
            try {
                Thread.sleep(1000); // Time to recompile
            } catch (InterruptedException ex) {
                throw new RuntimeException(ex);
            }
            return whiteBox.isMethodCompiled(testMethodRef);
        });
    }

    private static void timedDoWhile(String name, BooleanSupplier action) {
        final var startTime = System.nanoTime();
        boolean completed;
        do {
            assertLessThan((System.nanoTime() - startTime) / 1_000_000_000, STAGE_TIME_LIMIT_SEC,
                "Task takes too long: " + name
            );
            System.out.println("Running: " + name);
            completed = action.getAsBoolean();
        } while (!completed);
        System.out.println("Completed: " + name);
    }
}
