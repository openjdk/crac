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

import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.crac.CracContainerBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

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
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest    -  -  -  -
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest 128M  -  -  -
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest 128M  - 1G  -
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest 128M  - 4G  -
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest 128M  - 1G  -
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest 128M 80 1G  -
 * @run driver/timeout=120 jdk.test.lib.crac.CracTest 128M  - 1G 80
 */
public class ContainerOOMETest implements CracTest {
    private static final String AFTER_OOME = "AFTER OOME";

    @CracTestArg(0)
    String checkpointHeapLimit;

    @CracTestArg(1)
    String checkpointMaxRAMPercentage;

    @CracTestArg(2)
    String restoreContainerMemory;

    @CracTestArg(3)
    String restoreMaxRAMPercentage;

    @Override
    public void test() throws Exception {
        DockerTestUtils.checkCanUseResourceLimits();
        final String imageName = Common.imageName("oome-test");
        CracContainerBuilder builder = new CracContainerBuilder()
                .inDockerImage(imageName)
                .dockerOptions("-m", "256M")
                .runContainerDirectly(true)
                .containerUsePrivileged(true)
                // Without specific request for G1 we would get Serial on 256M
                .vmOption("-XX:+UseG1GC")
                .vmOption("-Xmx4G");
        addVmOption(builder, "-XX:CRaCMaxHeapSizeBeforeCheckpoint=", checkpointHeapLimit);
        addVmOption(builder, "-XX:MaxRAMPercentage=", checkpointMaxRAMPercentage);
        if ("-".equals(checkpointHeapLimit)) {
            // Killed by Docker's OOME killer without JVM catching this
            builder.doCheckpointToAnalyze()
                    .shouldHaveExitValue(137)
                    .stderrShouldNotContain(AFTER_OOME);
            return;
        }
        try {
            builder.doCheckpointToAnalyze()
                    .shouldHaveExitValue(137) // checkpoint
                    .stderrShouldContain(AFTER_OOME);
            builder.clearDockerOptions().clearVmOptions();
            if (!"-".equals(restoreContainerMemory)) {
                builder.dockerOptions("-m", restoreContainerMemory);
            }
            addVmOption(builder, "-XX:MaxRAMPercentage=", restoreMaxRAMPercentage);
            builder.doRestoreToAnalyze().shouldHaveExitValue(0);
        } finally {
            builder.ensureContainerKilled();
        }
    }

    private void addVmOption(CracContainerBuilder builder, String option, String value) {
        if (!"-".equals(value)) {
            builder.vmOption(option + value);
        }
    }

    @Override
    public void exec() throws Exception {
        assertGreaterThan(lotOfAllocations(5_000_000), 0, "Should have OOMEd");
        System.err.println(AFTER_OOME);
        CRaCMXBean.getCRaCMXBean().checkpointRestore();
        int allocationsLeft = lotOfAllocations(5_000_000);
        if ("-".equals(restoreContainerMemory) || "4G".equals(restoreContainerMemory)) {
            // no restraints - allocations should succeed (assuming >= 4GB physical RAM on testing machine)
            assertEquals(0, allocationsLeft);
        } else if (!"-".equals(checkpointMaxRAMPercentage) || !"-".equals(restoreMaxRAMPercentage)) {
            // With non-default MaxRAMPercentage it should fit into memory
            assertEquals(0, allocationsLeft);
        } else if ("1G".equals(restoreContainerMemory)) {
            // 1 GB, default MaxRAMPercentage = 25% => allocations shouldn't succeed
            assertGreaterThan(allocationsLeft, 0, "Should have OOMEd");
        } else {
            fail("Unexpected memory size");
        }
    }

    private static int lotOfAllocations(int countdown) {
        HashMap<String, String> map = new HashMap<>();
        try {
            for (; countdown > 0; --countdown) {
                // Same width for each instance: 80 bytes = 24 String, 24 byte[], 32 HashMap$Node
                String str = String.format("%08x", countdown);
                map.put(str, str);
            }
        } catch (OutOfMemoryError e) {
            // ignore the error, just return
        }
        return countdown;
    }
}
