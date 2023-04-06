/*
 * Copyright (c) 2022, Azul Systems, Inc. All rights reserved.
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

/*
 * @test RestoreEnvironmentTest
 * @summary the test checks that actual environment variables are propagated into a restored process.
 * @library /test/lib
 * @build RestoreEnvironmentTest
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest
 */
public class RestoreEnvironmentTest implements CracTest {
    static final String TEST_VAR_NAME = "RESTORE_ENVIRONMENT_TEST_VAR";
    static final String BEFORE_CHECKPOINT = "BeforeCheckpoint";
    static final String AFTER_RESTORE = "AfterRestore";
    static final String AFTER_SECOND_RESTORE = "AfterSecondRestore";
    static final String NEW_VALUE = "NewValue";
    public static final String PREFIX1 = "(after restore) ";
    public static final String PREFIX2 = "(after second restore) ";

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().captureOutput(true)
                .env(TEST_VAR_NAME + 0, BEFORE_CHECKPOINT)
                .env(TEST_VAR_NAME + 1, BEFORE_CHECKPOINT);
        builder.doCheckpoint();
        builder.env(TEST_VAR_NAME + 1, AFTER_RESTORE);
        builder.env(TEST_VAR_NAME + 2, NEW_VALUE);
        builder.startRestore().waitForCheckpointed().outputAnalyzer()
                .shouldContain(PREFIX1 + TEST_VAR_NAME + "0=" + BEFORE_CHECKPOINT)
                .shouldContain(PREFIX1 + TEST_VAR_NAME + "1=" + AFTER_RESTORE)
                .shouldContain(PREFIX1 + TEST_VAR_NAME + "2=" + NEW_VALUE);
        builder.env(TEST_VAR_NAME + 0, AFTER_SECOND_RESTORE);
        builder.env(TEST_VAR_NAME + 1, AFTER_SECOND_RESTORE);
        builder.doRestore().outputAnalyzer()
                .shouldContain(PREFIX2 + TEST_VAR_NAME + "0=" + AFTER_SECOND_RESTORE)
                .shouldContain(PREFIX2 + TEST_VAR_NAME + "1=" + AFTER_SECOND_RESTORE)
                .shouldContain(PREFIX2 + TEST_VAR_NAME + "2=" + NEW_VALUE);
    }

    @Override
    public void exec() throws Exception {
        printVars("(before checkpoint) ");
        jdk.crac.Core.checkpointRestore();
        printVars(PREFIX1);
        jdk.crac.Core.checkpointRestore();
        printVars(PREFIX2);
    }

    private static void printVars(String prefix) {
        for (int i = 0; i < 3; ++i) {
            var testVar = System.getenv(TEST_VAR_NAME + i);
            System.out.println(prefix + TEST_VAR_NAME + i + "=" + testVar);
        }
    }
}
