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

import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;

/**
 * @test Testing CracEngineOptions is not influenced by obsoleted CRAC_CRIU_OPTS env variable.
 * @comment The remaining warning should be dropped together with this test in JDK 29.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build CracCriuOptsTest
 * @run driver jdk.test.lib.crac.CracTest
 */

public class CracCriuOptsTest implements CracTest {
    private static final String RESTORED = "RESTORED";

    @Override
    public void test() throws Exception {
        if (Runtime.version().feature() >= 29) {
            throw new IllegalStateException("This test should be removed together with all CRAC_CRIU_* mentions in JDK 29");
        }
        final CracBuilder builder = new CracBuilder().engine(CracEngine.CRIU)
                .engineOptions("print_command=true")
                .env("CRAC_CRIU_OPTS", "-R");
        builder.doCheckpointToAnalyze()
                .shouldHaveExitValue(137)
                .stderrShouldContain("CRAC_CRIU_OPTS is obsolete and has no impact")
                .stderrShouldNotContain(RESTORED);
    }

    @Override
    public void exec() throws Exception {
        CRaCMXBean.getCRaCMXBean().checkpointRestore();
        System.err.println(RESTORED);
    }
}
