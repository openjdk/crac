/*
 * Copyright (c) 2024, Azul Systems, Inc. All rights reserved.
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
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.crac.CracContainerBuilder;
import jdk.test.lib.crac.CracTest;

import java.util.HashMap;

import static jdk.test.lib.Asserts.*;

/*
 * @test ContainerOOMETest
 * @requires (os.family == "linux")
 * @requires container.support
 * @comment Static JDK eagerly loads X11 which is missing from the Docker image
 * @requires !jdk.static
 * @library /test/lib
 * @modules java.base/jdk.internal.platform
 * @build ContainerOOMETest
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest
 */
public class ContainerOOMETest implements CracTest {
    private static final String AFTER_OOME = "AFTER OOME";

    @Override
    public void test() throws Exception {
        DockerTestUtils.checkCanUseResourceLimits();
        final String imageName = Common.imageName("oome-test");
        CracContainerBuilder builder = new CracContainerBuilder()
                .captureOutput(true)
                .inDockerImage(imageName)
                .dockerOptions("-m", "256M")
                .runContainerDirectly(true)
                // Without specific request for G1 we would get Serial on 256M
                .vmOption("-XX:+UseG1GC")
                .vmOption("-Xmx4G")
                .vmOption("-XX:CRaCMaxHeapSizeBeforeCheckpoint=128M");
        try {
            builder.startCheckpoint().outputAnalyzer()
                    .shouldHaveExitValue(137) // checkpoint
                    .stderrShouldContain(AFTER_OOME);
            builder.clearDockerOptions().clearVmOptions().captureOutput(false);
            builder.doRestore();
        } finally {
            builder.ensureContainerKilled();
        }
    }

    @Override
    public void exec() throws Exception {
        try {
            lotOfAllocations();
            fail("Should have OOMEd");
        } catch (OutOfMemoryError error) {
            // ignore the error
        }
        System.err.println(AFTER_OOME);
        Core.checkpointRestore();
        lotOfAllocations();
    }

    private static void lotOfAllocations() {
        HashMap<String, String> map = new HashMap<>();
        for (int i = 0; i < 10000000; ++i) {
            String str = String.valueOf(i);
            map.put(str, str);
            if (i % 1000000 == 0) {
                System.err.println(i + ": " + Runtime.getRuntime().totalMemory());
            }
        }
    }
}
