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
 * Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale
 * CA 94089 USA or visit www.azul.com if you need additional information or
 * have any questions.
 */

import jdk.crac.Core;
import jdk.test.lib.Platform;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;
import jdk.test.lib.util.FileUtils;

import static jdk.test.lib.Asserts.*;

/*
 * @test
 * @summary Check combinations of CPUFeatures and CheckCPUFeatures VM options
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build CheckCPUFeaturesTest
 * @run driver jdk.test.lib.crac.CracTest native  compatible        pass
 * @run driver jdk.test.lib.crac.CracTest native  exact             pass
 * @run driver jdk.test.lib.crac.CracTest native  skip              fail
 * @run driver jdk.test.lib.crac.CracTest native  skip-experimental pass
 * @run driver jdk.test.lib.crac.CracTest generic compatible        pass
 * @run driver jdk.test.lib.crac.CracTest generic exact             fail-x86
 * @run driver jdk.test.lib.crac.CracTest generic skip              fail
 * @run driver jdk.test.lib.crac.CracTest generic skip-experimental pass
 * @run driver jdk.test.lib.crac.CracTest ignore  compatible        fail-x86
 * @run driver jdk.test.lib.crac.CracTest ignore  exact             fail-x86
 * @run driver jdk.test.lib.crac.CracTest ignore  skip              fail
 * @run driver jdk.test.lib.crac.CracTest ignore  skip-experimental pass
 */
public class CheckCPUFeaturesTest implements CracTest {
    @CracTestArg(0)
    private String features;

    @CracTestArg(1)
    private String check;

    @CracTestArg(2)
    private String result;

    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.PAUSE);
        if (builder.imageDir().toFile().exists()) {
            FileUtils.deleteFileTreeWithRetry(builder.imageDir());
        }
        CracProcess checkpoint = builder.vmOption("-XX:CPUFeatures=" + features).startCheckpoint();
        checkpoint.waitForPausePid();

        builder.clearVmOptions();
        if ("skip-experimental".equals(check)) {
            builder.vmOption("-XX:+UnlockExperimentalVMOptions");
            check = "skip";
        }
        CracProcess restore = builder.vmOption("-XX:CheckCPUFeatures=" + check).startRestore();
        boolean success = "pass".equals(result);
        if (!Platform.isX86() && !Platform.isX64()) {
            success = success || "fail-x86".equals(result);
        }
        if (success) {
            restore.waitForSuccess();
            checkpoint.waitForSuccess();
        } else {
            assertEquals(1, restore.waitFor());
            checkpoint.destroyForcibly();
        }
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
    }
}
