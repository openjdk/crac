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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

import jdk.crac.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import static jdk.test.lib.Asserts.*;

/**
 * @test Testing CracEngineOptions influenced by CRAC_CRIU_OPTS env variable.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build CracCriuOptsTest
 * @run driver jdk.test.lib.crac.CracTest PREREQ_CHECK
 * @run driver jdk.test.lib.crac.CracTest NOT_SET
 * @run driver jdk.test.lib.crac.CracTest ENVVAR_USED
 * @run driver jdk.test.lib.crac.CracTest ALREADY_SET
 */

public class CracCriuOptsTest implements CracTest {
    private static final String CRAC_CRIU_OPTS = "CRAC_CRIU_OPTS";
    private static final Path LOG_FILE_PATH = Path.of("restore.log");

    public enum Variant {
        PREREQ_CHECK,
        NOT_SET,
        ENVVAR_USED,
        ALREADY_SET
    }

    @CracTestArg
    Variant variant;

    @Override
    public void test() throws Exception {
        final CracBuilder builder = new CracBuilder();
        builder.doCheckpoint();

        // "direct_map=false" engine option is expected to add "--no-mmap-page-image" to
        // CRAC_CRIU_OPTS — this is what we'll check.
        // PREREQ_CHECK checks that the test's pre-requisite has not changed: when neither
        // direct_map=true nor --no-mmap-page-image is specified direct mapping IS performed.
        final boolean disableDirectMap = variant != Variant.PREREQ_CHECK;
        if (variant == Variant.ENVVAR_USED) {
            builder.env(CRAC_CRIU_OPTS, "-v");
        } else if (variant == Variant.ALREADY_SET) {
            builder.env(CRAC_CRIU_OPTS, "-v --no-mmap-page-image");
        }
        // Using log file instead of stderr because output capturing takes too long for some reason.
        // If we let CRIU create the file we may not get reading permissions, so we create it by
        // ourselves and also truncate it if it exists from previous runs (CRIU should truncate
        // automatically but just to be safe).
        Files.write(LOG_FILE_PATH, new byte[0]); // Creates if not exists, truncates otherwise
        builder.engineOptions("args=-v4 -o " + LOG_FILE_PATH + (disableDirectMap ? ",direct_map=false" : ""));

        builder.doRestore();
        checkLogFile(!disableDirectMap);
    }

    private static void checkLogFile(boolean directMap) throws IOException {
        // CRIU prints debug lines "Preadv %lx:%d... (%d iovs) (mmap %d)" where the last %d is
        // either 0 or 1 depending on whether direct mapping is performed
        final var lines = Files.lines(LOG_FILE_PATH).filter(s ->
            s.matches("\\(\\d+.\\d+\\) pie: \\d+: Preadv 0x\\p{XDigit}+:\\d+\\.\\.\\. \\(\\d+ iovs\\) \\(mmap [01]\\)")
        ).toList();
        assertFalse(lines.isEmpty(), "At least one log line must match the expected pattern");
        for (var s : lines) {
            // If this fails it means that direct_map is on when it should be off or vise-versa
            final var end = "(mmap " + (directMap ? "1" : "0") + ")";
            assertTrue(s.endsWith(end), "Log line \"" + s + "\" must end with \"" + end + "\"");
        }
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
    }
}
