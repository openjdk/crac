/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.nio.file.Files;
import java.nio.file.Path;

/**
 * @test
 * @requires os.family == "linux"
 * @library /test/lib
 * @build RepeatedCheckpointTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class RepeatedCheckpointTest implements CracTest {
    private static final int NUM_CHECKPOINTS = 3;
    private static final Path DO_CHECKPOINTS_MARKER = Path.of("do-checkpoints-marker");

    @Override
    public void test() throws Exception {
        Files.createFile(DO_CHECKPOINTS_MARKER);
        new CracBuilder().imageDir("cr%g").engineOptions("keep_running=true")
                .doCheckpointToAnalyze().shouldHaveExitValue(0);

        Files.delete(DO_CHECKPOINTS_MARKER); // Do not create new checkpoints, we want to test the ones already created
        final var restoreBuilder = new CracBuilder();
        for (int i = 0; i < NUM_CHECKPOINTS; i++) {
            restoreBuilder.imageDir("cr" + (i + 1)).doRestore(); // %g starts from 1
        }
    }

    @Override
    public void exec() throws Exception {
        final var mxBean = CRaCMXBean.getCRaCMXBean();
        for (int i = 0; i < NUM_CHECKPOINTS && Files.exists(DO_CHECKPOINTS_MARKER); i++) {
            System.out.println("Checkpoint #" + (i + 1));
            mxBean.checkpointRestore();
        }
        System.out.println("Completed");
    }
}
