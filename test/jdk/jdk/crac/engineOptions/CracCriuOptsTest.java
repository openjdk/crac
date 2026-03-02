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
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import static jdk.test.lib.Asserts.*;

/**
 * @test Testing CracEngineOptions influenced by CRAC_CRIU_OPTS env variable.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build CracCriuOptsTest
 * @run driver jdk.test.lib.crac.CracTest NOT_SET
 * @run driver jdk.test.lib.crac.CracTest ENVVAR_USED
 * @run driver jdk.test.lib.crac.CracTest ALREADY_SET
 */

public class CracCriuOptsTest implements CracTest {
    private static final String CRAC_CRIU_OPTS = "CRAC_CRIU_OPTS";
    private static final String RESTORED = "RESTORED";

    public enum Variant {
        NOT_SET,
        ENVVAR_USED,
        ALREADY_SET
    }

    @CracTestArg
    Variant variant;

    @Override
    public void test() throws Exception {
        final CracBuilder builder = new CracBuilder().engine(CracEngine.CRIU)
                .engineOptions("keep_running=true,print_command=true");
        if (variant == Variant.ENVVAR_USED) {
            builder.env(CRAC_CRIU_OPTS, "-v");
        } else if (variant == Variant.ALREADY_SET) {
            builder.env(CRAC_CRIU_OPTS, "-v -R");
        }
        builder.doCheckpointToAnalyze()
                .shouldHaveExitValue(0)
                .stderrShouldContain(RESTORED);

        builder.engineOptions().doRestore();
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
        System.err.println(RESTORED);
    }
}
