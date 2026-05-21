/*
 * Copyright (c) 2025, 2026, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.process.OutputAnalyzer;

import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;

import static jdk.test.lib.Asserts.assertFalse;
import static jdk.test.lib.Asserts.assertTrue;

/**
 * @test
 * @summary If a restore attempt failed and was ignored by
 *          CRaCIgnoreRestoreIfUnavailable the normal execution started instead
 *          should be checkpointable.
 *          Also tests CRaCCheckpointEngineOptions and CRaCRestoreEngineOptions.
 * @requires (os.family == "linux")
 * @library /test/lib
 */
public class CheckpointAfterIgnoredRestoreTest {
    public static void main(String[] args) throws Exception {
        final var builder = new CracBuilder().engine(CracEngine.CRIU);
        assertTrue(Files.notExists(builder.imageDir()), "Image should not exist yet: " + builder.imageDir());
        builder.forwardClasspathOnRestore(true)
                // CRaCRestoreFrom will be added automatically
                .vmOption("-XX:CRaCCheckpointTo=" + builder.imageDir())
                .vmOption("-XX:+CRaCIgnoreRestoreIfUnavailable")
                .vmOption("-XX:CRaCEngineOptions=print_command=true")
                // -v4 and -v1 are used by default
                .vmOption("-XX:CRaCCheckpointEngineOptions=args=-v2")
                .vmOption("-XX:CRaCRestoreEngineOptions=args=-v3");

        final OutputAnalyzer checkpointOut;
        try (final var p = builder.startRestoreWithArgs(List.of(), List.of(Main.class.getName()))) {
            p.waitForCheckpointed();
            checkpointOut = p.outputAnalyzer();
        }
        checkpointOut.shouldNotContain("[warning]");
        checkArgs(checkpointOut, "-v2", "-v3");

        final OutputAnalyzer restoreOut = builder.doRestoreToAnalyze();
        restoreOut.shouldHaveExitValue(0).shouldNotContain("[warning]");
        checkArgs(restoreOut, "-v3", "-v2");
    }

    private static void checkArgs(OutputAnalyzer outputAnalyzer, String expected, String unexpected) {
        final var commandLog = outputAnalyzer.stderrAsLines().stream()
                .filter(l -> l.contains("criuengine: Command: "))
                .findFirst().orElseThrow();
        final var command = Arrays.asList(commandLog.split("criuengine: Command: ", 2)[1].split("\\s+"));
        assertTrue(command.contains(expected), "'" + expected + "' not found in " + command);
        assertFalse(command.contains(unexpected), "'" + unexpected + "' found in " + command);
    }

    public static class Main {
        public static void main(String[] args) throws Exception {
            CRaCMXBean.getCRaCMXBean().checkpointRestore();
        }
    }
}
