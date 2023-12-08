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


import jdk.crac.*;

import jdk.test.lib.Asserts;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.util.Arrays;

import static jdk.test.lib.Asserts.*;

/**
 * @test CracTimeoutTest
 * @library /test/lib
 * @build CracTimeoutTest
 * @run driver jdk.test.lib.crac.CracTest
 * @requires (os.family == "linux")
 */

public class CracTimeoutTest implements CracTest {
    public static final String EXCEPTION_MESSAGE = "Native checkpoint failed.";
    public static final String TIMEOUT_MESSAGE = "The checkpoint was not generated within 10 seconds.\n" +
                                   "You can change the time-out period by -XX:CRaCCheckpointTimeout=.";

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().env("CRAC_CRIU_OPTS", "-V")
                .vmOption("-XX:CRaCCheckpointTimeout=10").captureOutput(true);
        builder.startCheckpoint().waitForSuccess().outputAnalyzer().shouldContain(TIMEOUT_MESSAGE);
    }

    @Override
    public void exec() throws Exception {
        long startTime = 0;
        try {
            startTime = System.currentTimeMillis();
            Core.checkpointRestore();
            fail("Was supposed to throw");
        } catch (CheckpointException e) {
            long endTime = System.currentTimeMillis();
            assertEquals(1, e.getSuppressed().length, Arrays.toString(e.getSuppressed()));
            assertEquals(EXCEPTION_MESSAGE, e.getSuppressed()[0].getMessage());
            long elapsedTime = endTime - startTime;
            assertTrue(elapsedTime >= 10000 && elapsedTime <= 11000, "Elapsed time is not between 10 and 11 seconds.");
        }
    }
}
