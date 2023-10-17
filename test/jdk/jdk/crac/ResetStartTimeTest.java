/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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
import jdk.test.lib.Utils;

import java.lang.management.ManagementFactory;
import java.nio.file.Path;
import java.util.*;

import static jdk.test.lib.Asserts.assertLessThan;

/**
 * @test
 * @library /test/lib
 * @build SimpleTest
 * @requires (os.family == "linux")
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest false
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest true
 */
public class SimpleTest implements CracTest {

    @CracTestArg(0)
    boolean resetUptime;

    static private final long WAIT_TIMEOUT = 2 * 1000; // msecs

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder();
        builder.startCheckpoint().waitForCheckpointed();
        if (resetUptime) {
            builder.vmOption("-XX:+CRaCResetStartTime");
        }
        builder.captureOutput(true).doRestore().outputAnalyzer().shouldContain(RESTORED_MESSAGE);
    }

    @Override
    public void exec() throws Exception {
        Thread.sleep(WAIT_TIMEOUT);
        final long uptime0 = ManagementFactory.getRuntimeMXBean().getUptime();

        Core.checkpointRestore();
        System.out.println(RESTORED_MESSAGE);

        final long uptime1 = ManagementFactory.getRuntimeMXBean().getUptime();

        if (resetUptime) {
            assertLessThan(uptime1, uptime0);
            assertLessThan(uptime1, WAIT_TIMEOUT);
        } else {
            assertLessThan(uptime0, uptime1);
        }
    }
}
