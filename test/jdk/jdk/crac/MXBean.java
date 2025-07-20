/*
 * Copyright (c) 2022, 2025, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.*;
import jdk.crac.management.*;

import jdk.test.lib.Platform;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.process.OutputAnalyzer;

import java.time.Instant;
import java.time.ZoneId;
import java.time.format.DateTimeFormatter;

import static jdk.test.lib.Asserts.*;

/**
 * @test
 * @library /test/lib
 * @build MXBean
 * @run driver jdk.test.lib.crac.CracTest
 */
public class MXBean implements CracTest {
    static final long TIME_TOLERANCE = 1000; // ms

    private static String formatTime(long t) {
        return DateTimeFormatter.ofPattern("E dd LLL yyyy HH:mm:ss.n").format(
                Instant.ofEpochMilli(t).atZone(ZoneId.systemDefault()));
    }

    @Override
    public void exec() throws CheckpointException, RestoreException {
        CRaCMXBean cracMXBean = CRaCMXBean.getCRaCMXBean();

        Core.checkpointRestore();

        System.out.println("UptimeSinceRestore " + cracMXBean.getUptimeSinceRestore());

        long restoreTime = cracMXBean.getRestoreTime();
        System.out.println("RestoreTime " + restoreTime + " " + formatTime(restoreTime));
    }

    @Override
    public void test() throws Exception {
        final var builder = new CracBuilder().captureOutput(true);

        final OutputAnalyzer output;
        final long restoreStartTime;
        if (Platform.isLinux()) { // Linux is currently the only platform supporting non-immediate restore
            final var process = builder.engine(CracEngine.PAUSE).startCheckpoint();
            output = process.outputAnalyzer();
            process.waitForPausePid();
            restoreStartTime = System.currentTimeMillis();
            builder.doRestore();
        } else {
            restoreStartTime = System.currentTimeMillis(); // A very rough approximation
            output = builder.engine(CracEngine.SIMULATE).startCheckpoint().waitForSuccess().outputAnalyzer();
        }
        System.out.println("RestoreStartTime " + restoreStartTime + " " + formatTime(restoreStartTime));

        final long uptimeSinceRestore = Long.parseLong(output.firstMatch("UptimeSinceRestore ([0-9-]+)", 1));
        assertGTE(uptimeSinceRestore, 0L, "Bad UptimeSinceRestore");
        assertLT(uptimeSinceRestore, TIME_TOLERANCE, "UptimeSinceRestore should be a bit greater than 0");

        final long restoreTime = Long.parseLong(output.firstMatch("RestoreTime ([0-9-]+)", 1));
        assertGTE(restoreTime, 0L, "Bad RestoreTime");
        if (Platform.isLinux()) {
            assertLT(restoreTime - restoreStartTime, TIME_TOLERANCE,
                    "RestoreTime " + restoreTime + " should be a bit greater than " + restoreStartTime);
        } else {
            assertGT(restoreTime, restoreStartTime, "Time has gone backwards?");
        }
    }
}
