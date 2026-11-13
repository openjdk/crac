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
 * Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale
 * CA 94089 USA or visit www.azul.com if you need additional information or
 * have any questions.
 */
import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.crac.*;

import java.io.File;
import java.nio.file.Path;

/*
 * @test
 * @summary Check the jdk.crac.labels and jdk.crac.require-labels functionality
 * @library /test/lib
 * @build RequiredLabelsTest
 * @comment simengine with pause=true is available only on Linux
 * @requires os.family == "linux"
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest foo=bar  foo=bar  true
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest foo=bar  -        true
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest -        foo=bar  false
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest foo=bar  foo=goo  false
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest foo=bar  foo=$BAR false
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest foo=$BAR foo=GOO  true
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest foo      foo=xxx  true
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest -        foo      false
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest foo,abc=def,ghi=$BAR abc=def,ghi=$BAR,foo=xxx true
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest foo,abc=def,ghi=$BAR abc=xxx,foo              false
 * @run driver/timeout=15 jdk.test.lib.crac.CracTest not_defined_env,abc=$NOT_DEFINED_ENV not_defined_env,abc=$NOT_DEFINED_ENV true
 */
public class RequiredLabelsTest implements CracTest {
    @CracTestArg(0)
    String setLabels;

    @CracTestArg(1)
    String requireLabels;

    @CracTestArg(2)
    boolean shouldSucceed;

    private static CracBuilder newPauseBuilder() {
        return new CracBuilder().engine(CracEngine.SIMULATE).engineOptions("pause=true")
                .env("BAR", "GOO")
                .env("foo", "xxx");
    }

    @Override
    public void test() throws Exception {
        CracBuilder cpBuilder = newPauseBuilder();
        if (!"-".equals(setLabels)) {
            cpBuilder.vmOption("-XX:CRaCImageLabels=" + setLabels);
        }
        // Ensure that the pid file does not exist as leftover from previous test
        //noinspection ResultOfMethodCallIgnored
        cpBuilder.imageDir().resolve("pid").toFile().delete();

        try (var cp = cpBuilder.startCheckpoint()) {
            cp.waitForPausePid();

            CracBuilder restoreBuilder = newPauseBuilder();
            if (!"-".equals(requireLabels)) {
                restoreBuilder.vmOption("-XX:CRaCRequiredImageLabels=" + requireLabels);
            }
            var restore = restoreBuilder.doRestoreToAnalyze();
            if (shouldSucceed) {
                restore.shouldHaveExitValue(0);
            } else {
                restore.shouldNotHaveExitValue(0);
                newPauseBuilder().doRestore();
            }
            cp.waitForSuccess();
        }
    }

    @Override
    public void exec() throws Exception {
        CRaCMXBean.getCRaCMXBean().checkpointRestore();
    }
}
