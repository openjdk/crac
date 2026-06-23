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
import jdk.test.lib.Utils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.util.FileUtils;

/*
 * @test
 * @summary Check combinations of CPUFeatures and CheckCPUFeatures VM options
 * @requires (os.family == "linux")
 * @requires os.arch=="amd64" | os.arch=="x86_64"
 * @library /test/lib
 * @build CheckCPUFeaturesMessageTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class CheckCPUFeaturesMessageTest implements CracTest {
    private static final String GENERIC_FEATURES_X86 = "0x4067,0x0";

    private void testDefault(CracBuilder builder) throws Exception {
        builder.doRestoreToAnalyze()
                .shouldHaveExitValue(1)
                .shouldContain("Restore failed due to incompatible or missing CPU features");
    }

    private void testPatterns(CracBuilder builder) throws Exception {
        ProcessBuilder pb = new ProcessBuilder(Utils.TEST_JDK + "/bin/java", "-XX:+ShowCPUFeatures", "--version");
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        output.shouldHaveExitValue(0);
        String currentCpuFeatures = output.getStdout().lines()
                .filter(line -> line.startsWith("CPU features being used are") && line.contains("="))
                .map(line -> line.substring(line.lastIndexOf("=") + 1))
                .findFirst().orElseThrow();
        builder.vmOption("-XX:CheckCPUFeaturesMessage=%cMyError%sMessageWith%%Common%m")
                .doRestoreToAnalyze()
                .shouldHaveExitValue(1)
                .shouldContain(currentCpuFeatures + "MyError" + GENERIC_FEATURES_X86 + "MessageWith%Common0x");
    }

    private void testQuiet(CracBuilder builder) throws Exception {
        builder.vmOption("-Xlog:crac=Off")
                .doRestoreToAnalyze()
                .shouldHaveExitValue(1)
                .shouldNotContain("CRaC engine failed to restore");
    }

    private CracBuilder mustFail(CracBuilder builder) {
        return builder.clearVmOptions().vmOption("-XX:CheckCPUFeatures=exact");
    }

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder()
                .engine(CracEngine.SIMULATE).engineOptions("pause=true")
                .vmOption("-XX:CPUFeatures=generic");
        if (builder.imageDir().toFile().exists()) {
            FileUtils.deleteFileTreeWithRetry(builder.imageDir());
        }

        try (var checkpoint = builder.startCheckpoint()) {
            checkpoint.waitForPausePid();

            testDefault(mustFail(builder));
            testPatterns(mustFail(builder));
            testQuiet(mustFail(builder));

            builder.clearVmOptions().doRestore();
            checkpoint.waitForSuccess();
        }
    }

    @Override
    public void exec() throws Exception {
        CRaCMXBean.getCRaCMXBean().checkpointRestore();
    }
}
