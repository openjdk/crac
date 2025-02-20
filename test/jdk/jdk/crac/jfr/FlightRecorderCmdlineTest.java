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
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.File;

import static jdk.test.lib.Asserts.assertTrue;

/**
 * @test FlightRecorderCmdlineTest
 * @library /test/lib
 * @requires (os.family == "linux")
 * @build FlightRecorderTestBase
 * @build FlightRecorderCmdlineTest
 * @run driver jdk.test.lib.crac.CracTest true false
 * @run driver jdk.test.lib.crac.CracTest false true
 * @run driver jdk.test.lib.crac.CracTest true true
 *
 */
public class FlightRecorderCmdlineTest extends FlightRecorderTestBase implements CracTest {
    @CracTestArg(0)
    boolean beforeCheckpoint;

    @CracTestArg(1)
    boolean afterRestore;

    @Override
    public void test() throws Exception {
        File jfrOne = File.createTempFile("one", ".jfr");
        File jfrTwo = File.createTempFile("two", ".jfr");
        assertTrue(jfrOne.delete());
        assertTrue(jfrTwo.delete());
        CracBuilder cpBuilder = new CracBuilder();
        cpBuilder.vmOption("-Xlog:jfr=debug");
        if (beforeCheckpoint) {
            cpBuilder.vmOption("-XX:StartFlightRecording=dumponexit=true,filename=" + jfrOne);
        }
        cpBuilder.doCheckpoint();
        if (beforeCheckpoint) {
            assertRecording(jfrOne);
        }
        CracBuilder rsBuilder = new CracBuilder();
        if (afterRestore) {
            rsBuilder.vmOption("-XX:StartFlightRecording=dumponexit=true,filename=" + jfrTwo);
        }
        rsBuilder.doRestore();
        if (beforeCheckpoint) {
            assertRecording(jfrOne);
        }
        if (afterRestore) {
            assertRecording(jfrTwo);
        }
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
    }
}
