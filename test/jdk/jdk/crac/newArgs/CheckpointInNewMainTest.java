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

import java.util.List;

import jdk.crac.Core;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;

/**
 * @test
 * @summary It should be possible to C/R in a new main.
 * @library /test/lib
 * @build CheckpointInNewMainTest
 * @run driver jdk.test.lib.crac.CracTest
 * @requires (os.family == "linux")
 */
public class CheckpointInNewMainTest implements CracTest {
    private static final String NEW_MAIN_CLASS = "CheckpointInNewMainTest$InternalMain";
    private static final String RESTORE_OLD_MSG = "RESTORED IN OLD MAIN";
    private static final String RESTORE_NEW_MSG = "RESTORED IN NEW MAIN";

    @Override
    public void test() throws Exception {
        final var builder = new CracBuilder().captureOutput(true)
            // Disabling direct_map to be able to overwrite the first image,
            // otherwise the second image will depend on the first one and
            // we will get CRIU errors on the second restore
            .engine(CracEngine.CRIU).vmOption("-XX:CRaCEngineOptions=direct_map=false");

        // Checkpoint in the old main
        builder.doCheckpoint();
        // Restore from the old main and checkpoint in the new main
        builder.startRestoreWithArgs(null, List.of(NEW_MAIN_CLASS)).waitForCheckpointed();
        // Restore from the new main
        final var out = builder.doRestore().outputAnalyzer();

        out.shouldContain(RESTORE_OLD_MSG).shouldContain(RESTORE_NEW_MSG);
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
        System.out.println(RESTORE_OLD_MSG);
    }

    public static class InternalMain {
        public static void main(String[] args) throws Exception {
            Core.checkpointRestore();
            System.out.println(RESTORE_NEW_MSG);
        }
    }
}
