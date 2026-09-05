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
import jdk.test.lib.crac.CracTestArg;

/**
 * @test
 * @requires os.family == "linux"
 * @library /test/lib
 * @build RepeatedCheckpointTest
 * @run driver jdk.test.lib.crac.CracTest false
 * @run driver jdk.test.lib.crac.CracTest true
 */
public class RepeatedCheckpointTest implements CracTest {
    private static final int NUM_CHECKPOINTS = 10;

    @CracTestArg(0)
    boolean changeImageLocation;

    @Override
    public void test() throws Exception {
        final var builder = new CracBuilder().imageDir("cr0");
        builder.doCheckpoint();
        for (int i = 1; i < NUM_CHECKPOINTS; i++) {
            final var nextImageLocation = changeImageLocation ? "cr" + i : builder.imageDir().toString();
            builder.clearVmOptions().vmOption("-XX:CRaCCheckpointTo=" + nextImageLocation);
            try (final var p = builder.startRestore()) {
                p.waitForCheckpointed();
            }
            builder.imageDir(nextImageLocation);
        }
        builder.clearVmOptions().doRestore();
    }

    @Override
    public void exec() throws Exception {
        final var mxBean = CRaCMXBean.getCRaCMXBean();
        for (int i = 0; i < NUM_CHECKPOINTS; i++) {
            mxBean.checkpointRestore();
        }
    }
}
