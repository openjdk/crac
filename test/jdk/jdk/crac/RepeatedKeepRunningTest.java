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
 * @build RepeatedKeepRunningTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class RepeatedKeepRunningTest implements CracTest {
    private static final int NUM_CHAINED_CHECKPOINTS = 3;
    private static final Path MAKE_CHECKPOINT_CHAIN_MARKER = Path.of("make-checkpoint-chain");
    private static final Path OUT_OF_CHAIN_IMAGE = Path.of("cr-ooc");

    @Override
    public void test() throws Exception {
        // N chained checkpoint/restores at (1)
        Files.createFile(MAKE_CHECKPOINT_CHAIN_MARKER);
        new CracBuilder().imageDir("cr%g").engineOptions("keep_running=true")
                .doCheckpointToAnalyze().shouldHaveExitValue(0);
        Files.delete(MAKE_CHECKPOINT_CHAIN_MARKER);

        final var builder = new CracBuilder().vmOption("-XX:CRaCCheckpointTo=" + OUT_OF_CHAIN_IMAGE);
        for (int i = 0; i < NUM_CHAINED_CHECKPOINTS; i++) {
            // Restore from a certain generation in the chain at (1), checkpoint/restore at (2)
            builder.imageDir("cr" + (i + 1) /* %g starts from 1 */).doRestore();
            // Restore from (2)
            builder.imageDir(OUT_OF_CHAIN_IMAGE.toString()).doRestore();
        }
    }

    @Override
    public void exec() throws Exception {
        final var mxBean = CRaCMXBean.getCRaCMXBean();

        for (int i = 0; i < NUM_CHAINED_CHECKPOINTS && Files.exists(MAKE_CHECKPOINT_CHAIN_MARKER); i++) {
            System.out.println("Checkpoint #" + (i + 1));
            mxBean.checkpointRestore(); // 1) N checkpoints without real restores in-between
        }

        if (!Files.exists(MAKE_CHECKPOINT_CHAIN_MARKER)) {
            System.out.println("Out-of-chain checkpoint");
            mxBean.checkpointRestore(); // 2) Checkpoint after a real restore from (1)
        }

        System.out.println("Completed");
    }
}
