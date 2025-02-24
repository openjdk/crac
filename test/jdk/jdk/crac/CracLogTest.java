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
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

/**
 * @test
 * @bug 8350569
 * @summary Checks that log set 'crac' can be manipulated via VM options.
 * @library /test/lib
 * @build CracLogTest
 * @run driver jdk.test.lib.crac.CracTest not-set
 * @run driver jdk.test.lib.crac.CracTest off
 * @run driver jdk.test.lib.crac.CracTest error
 * @run driver jdk.test.lib.crac.CracTest info
 * @run driver jdk.test.lib.crac.CracTest trace
 */
public class CracLogTest implements CracTest {
    private static final String CHECKPOINT_LOG_LEVEL = "info";
    private static final String CHECKPOINT_LOG_MSG = "Checkpoint ...";

    @CracTestArg(0)
    String logLevelStr;

    @Override
    public void test() throws Exception {
        var builder = new CracBuilder()
                .engine(CracEngine.SIMULATE)
                .captureOutput(true);
        if (!logLevelStr.equals("not-set")) {
            builder = builder.vmOption("-Xlog:crac=" + logLevelStr);
        }
        final var out = builder.startCheckpoint().waitForSuccess().outputAnalyzer();

        final var checkpointLogLevel = logLevelStrToInt(CHECKPOINT_LOG_LEVEL);
        final var selectedLogLevel = logLevelStrToInt(logLevelStr);
        if (checkpointLogLevel <= selectedLogLevel) {
            out.shouldContain(CHECKPOINT_LOG_MSG);
        } else {
            out.shouldNotContain(CHECKPOINT_LOG_MSG);
        }
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
    }

    private static int logLevelStrToInt(String str) {
        return switch (str) {
            case "not-set" -> logLevelStrToInt(CHECKPOINT_LOG_LEVEL);
            case "off" -> 0;
            case "error" -> 1;
            case "warning" -> 2;
            case "info" -> 3;
            case "debug" -> 4;
            case "trace" -> 5;
            default -> throw new IllegalArgumentException("Not a log level: " + str);
        };
    }
}
