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
 * Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale
 * CA 94089 USA or visit www.azul.com if you need additional information or
 * have any questions.
 */

import jdk.crac.Core;
import jdk.test.lib.crac.*;

import static jdk.test.lib.Asserts.*;

/*
 * @test
 * @summary pauseengine should pause the execution of the checkpointed process.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build PauseEngineTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class PauseEngineTest implements CracTest {
    private static final long PAUSE_TIME_MS = 5000;

    @Override
    public void test() throws Exception {
        final CracBuilder builder = new CracBuilder().engine(CracEngine.PAUSE);
        final CracProcess process = builder.startCheckpoint();
        process.waitForPausePid();
        Thread.sleep(PAUSE_TIME_MS);
        builder.doRestore();
        process.waitForSuccess();
    }

    @Override
    public void exec() throws Exception {
        final long before = System.nanoTime();
        Core.checkpointRestore();
        final long after = System.nanoTime();
        final long pauseTimeMs = (after - before) / 1_000_000;
        assertGTE(pauseTimeMs, PAUSE_TIME_MS,
            "Pause time is less than expected: " + pauseTimeMs + " < " + PAUSE_TIME_MS + " ms");
    }
}
