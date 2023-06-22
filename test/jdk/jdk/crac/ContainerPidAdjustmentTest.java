/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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

import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;
import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertLessThan;

/*
 * @test ContainerPidAdjustmentTest
 * @summary The test checks that process PID is adjusted with the specified value, when checkpointing in a container. Default min PID value is 128.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build ContainerPidAdjustmentTest
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   false  0       true   128
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   true   0       true   128
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   true   1       true   1
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   true   1000    false  1000
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   true   -10     true   128
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   true   40000   true   40000
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   true   40000   false  -1
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   true   5000000 true   -1
 */
public class ContainerPidAdjustmentTest implements CracTest {
    @CracTestArg(0)
    boolean needSetLastPid;

    @CracTestArg(1)
    long lastPid;

    @CracTestArg(2)
    boolean enablePrivileged;

    @CracTestArg(3)
    long expectedLastPid;

    @Override
    public void test() throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            return;
        }
        CracBuilder builder = new CracBuilder()
            .inDockerImage("pid-adjustment")
            .enablePrivilegesInContainer(enablePrivileged);
        if (needSetLastPid) {
            builder.dockerOptions("-e", "CRAC_MIN_PID=" + lastPid);
        }
        if (0 < expectedLastPid) {
            builder.startCheckpoint().waitForSuccess();
        } else {
            int expectedExitValue = (int)java.lang.Math.abs(expectedLastPid);
            int exitValue = builder.startCheckpoint().waitFor();
            assertEquals(expectedExitValue, exitValue, "Process returned unexpected exit code: " + exitValue);
        }
    }

    @Override
    public void exec() throws Exception {
        System.out.println("Current PID = " + ProcessHandle.current().pid());
        assertLessThan((long)0, expectedLastPid, "Shouldn't happen");
        assertLessThan(expectedLastPid, ProcessHandle.current().pid());
    }
}
