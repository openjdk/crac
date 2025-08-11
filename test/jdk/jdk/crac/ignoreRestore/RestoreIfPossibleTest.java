/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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

import java.nio.file.Files;
import java.util.Collections;
import java.util.List;

import jdk.crac.Core;

import static jdk.test.lib.Asserts.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.util.FileUtils;

/**
 * @test
 * @summary With CRaCIgnoreRestoreIfUnavailable specified, if the image in
 *          CRaCRestoreFrom can be restored from it should be used, otherwise
 *          main should be launched as usual.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @run main RestoreIfPossibleTest true
 * @run main RestoreIfPossibleTest false
 */
public class RestoreIfPossibleTest {
    private static final String WARMUP_MSG = "Running warmup workload...";
    private static final String MAIN_MSG = "Running main workload...";

    public static void main(String[] args) throws Exception {
        final var makeRestorePossible = Boolean.parseBoolean(args[0]);

        final var builder = new CracBuilder().engine(CracEngine.CRIU)
            .vmOption("-XX:+CRaCIgnoreRestoreIfUnavailable")
            .forwardClasspathOnRestore(true)
            .captureOutput(true);
        if (makeRestorePossible) {
            final var checkpointProcess = builder.main(Main.class).args("true").startCheckpoint();
            checkpointProcess.waitForCheckpointed();
            final var out = checkpointProcess.outputAnalyzer();
            out.stdoutShouldContain(WARMUP_MSG).stdoutShouldNotContain(MAIN_MSG);
        } else if (Files.exists(builder.imageDir())) { // Existance depends on the order of @run tags
            FileUtils.deleteFileTreeWithRetry(builder.imageDir());
        }

        final var out = builder.startRestoreWithArgs(null, List.of(Main.class.getName(), "false")).outputAnalyzer();
        out.stdoutShouldNotContain(WARMUP_MSG);

        // Check the count to ensure a new main is not launched on successful restore
        final var mainMsgCount = Collections.frequency(out.stdoutAsLines(), MAIN_MSG);
        if (mainMsgCount != 1) {
            out.reportDiagnosticSummary();
            assertEquals(1, mainMsgCount, "Main message should be printed exactly once");
        }
    }

    public static class Main {
        public static void main(String[] args) throws Exception {
            final var shouldCheckpoint = Boolean.parseBoolean(args[0]);
            if (shouldCheckpoint) {
                System.out.println(WARMUP_MSG);
                Core.checkpointRestore();
            }
            System.out.println(MAIN_MSG);
        }
    }
}
