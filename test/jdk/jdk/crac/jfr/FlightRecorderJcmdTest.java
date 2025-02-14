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


import jdk.crac.Core;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import java.io.File;

import static jdk.test.lib.Asserts.assertTrue;

/**
 * @test FlightRecorderJcmdTest
 * @library /test/lib
 * @requires (os.family == "linux")
 * @build FlightRecorderTestBase
 * @build FlightRecorderJcmdTest
 * @run driver jdk.test.lib.crac.CracTest
 *
 */
public class FlightRecorderJcmdTest extends FlightRecorderTestBase implements CracTest {
    protected static final String TEST_STARTED = "TEST STARTED";

    @Override
    public void test() throws Exception {
        File jfr = File.createTempFile("flight", ".jfr");
        assertTrue(jfr.delete());
        CracBuilder builder = new CracBuilder().captureOutput(true);
        CracProcess first = builder.startCheckpoint();
        first.waitForStdout(TEST_STARTED);
        builder.runJcmd(String.valueOf(first.pid()), "JFR.start", "name=xxx", "dumponexit=true", "filename=" + jfr)
                .shouldHaveExitValue(0)
                .shouldContain("Started recording");
        first.input().write('\n');
        first.input().flush();
        first.waitForCheckpointed();
        assertRecording(jfr);
        CracProcess second = new CracBuilder().captureOutput(true).startRestore();
        second.waitForStdout(RESTORED_MESSAGE, false);

        File jfrOther = File.createTempFile("other", ".jfr");
        assertTrue(jfrOther.delete());
        // CRIU restored with the same PID
        String restoredPid = String.valueOf(first.pid());
        builder.runJcmd(restoredPid, "JFR.dump", "name=xxx", "filename=" + jfrOther)
                .shouldHaveExitValue(0)
                .shouldContain("Dumped recording \"xxx\"");
        assertRecording(jfrOther);
        second.input().write('\n');
        second.input().flush();
        second.waitForSuccess();
        assertRecording(jfr);
    }

    @Override
    public void exec() throws Exception {
        System.out.println(TEST_STARTED);
        System.in.read();
        Core.checkpointRestore();
        System.out.println(RESTORED_MESSAGE);
        System.in.read();
    }
}
