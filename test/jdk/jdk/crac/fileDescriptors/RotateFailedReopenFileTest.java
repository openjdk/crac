/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTestArg;

import java.nio.file.Files;
import java.nio.file.Path;

import java.lang.management.ManagementFactory;
import javax.management.ObjectName;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "linux")
 * @summary Manual log rotation around checkpoint/restore must not crash
 *          when the log file cannot be reopened during restore.
 * @build RotateFailedReopenFileTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class RotateFailedReopenFileTest implements CracTest {
    private static final String POST_RESTORE_DONE = "Post-restore rotation done";
    private static final String LOG_REOPEN_FAILED = "Could not reopen log file";
    private static final String HOTSPOT_DIAGNOSTIC_MXBEAN_NAME =
        "com.sun.management:type=DiagnosticCommand";
    private static final String LOG_FILE_NAME = "gc.log";

    @CracTestArg(value = 0, optional = true)
    String logDir;

    @Override
    public void test() throws Exception {
        Path dir = Files.createTempDirectory("failed-reopen");
        Path log = dir.resolve(LOG_FILE_NAME);
        new CracBuilder()
                .engine(CracEngine.SIMULATE)
                .args(CracTest.args(dir.toString()))
                .vmOption("-Xlog:gc*=info:file=" + log + "::filecount=4")
                .doCheckpointToAnalyze()
                .shouldHaveExitValue(0)
                .shouldContain(LOG_REOPEN_FAILED)
                .shouldContain(POST_RESTORE_DONE);
    }

    @Override
    public void exec() throws Exception {
        // Delete log file and its dir, so reopen fails.
        Path dir = Path.of(logDir);
        Files.deleteIfExists(dir.resolve(LOG_FILE_NAME));
        Files.deleteIfExists(dir);
        CRaCMXBean.getCRaCMXBean().checkpointRestore();
        ObjectName name = new ObjectName(HOTSPOT_DIAGNOSTIC_MXBEAN_NAME);
        String[] vmLogArg = {"rotate"};
        Object[] dcmdArgs = {vmLogArg};
        String[] signature = {String[].class.getName()};
        ManagementFactory.getPlatformMBeanServer()
                .invoke(name, "vmLog", dcmdArgs, signature);
        System.out.println(POST_RESTORE_DONE);
    }
}
