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
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

/**
 * @test
 * @summary Tests System.exit() being called in the new main.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build ExitFromNewMainTest
 * @run driver jdk.test.lib.crac.CracTest true
 * @run driver jdk.test.lib.crac.CracTest false
 */
public class ExitFromNewMainTest implements CracTest {
    private static final String NEW_MAIN_CLASS = "ExitFromNewMainTest$InternalMain";
    private static final String RESTORE_OLD_MSG = "RESTORED IN OLD MAIN";
    private static final String RESTORE_NEW_MSG = "RESTORED IN NEW MAIN";

    @CracTestArg(0)
    boolean useExit;

    @Override
    public void test() throws Exception {
        final var builder = new CracBuilder().captureOutput(true);
        builder.doCheckpoint();

        final var out = builder
            .startRestoreWithArgs(null, List.of(NEW_MAIN_CLASS, Boolean.toString(useExit)))
            .waitForSuccess().outputAnalyzer();

        out.stdoutShouldContain(RESTORE_NEW_MSG);
        if (useExit) {
            out.stdoutShouldNotContain(RESTORE_OLD_MSG);
        } else {
            out.stdoutShouldContain(RESTORE_OLD_MSG);
        }
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
        System.out.println(RESTORE_OLD_MSG);
    }

    public static class InternalMain {
        public static void main(String[] args) {
            System.out.println(RESTORE_NEW_MSG);
            if (Boolean.parseBoolean(args[0])) {
                System.exit(0);
            }
        }
    }
}
