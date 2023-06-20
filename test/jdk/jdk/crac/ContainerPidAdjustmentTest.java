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
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;
import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertLessThan;

/*
 * @test ContainerPidAdjustmentTest
 * @summary The test checks that process PID is adjusted with the specified value, when checkpointing in a container. Default min PID value is 128.
 * @library /test/lib
 * @build ContainerPidAdjustmentTest
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest false 0      128  false
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest true  0      128  false
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest true  1      1    false
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest true  1000   1000 true
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest true  -10    128  false
 */
public class ContainerPidAdjustmentTest implements CracTest {
    @CracTestArg(0)
    boolean needSetLastPid;

    @CracTestArg(1)
    long lastPid;

    @CracTestArg(2)
    long expectedLastPid;

    @CracTestArg(3)
    boolean disablePrivileged;

    @Override
    public void test() throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            return;
        }
        CracBuilder builder = new CracBuilder()
            .inDockerImage("pid-adjustment")
            .disablePrivileged(disablePrivileged);
        if (needSetLastPid) {
            builder.dockerOptions("-e", "CRAC_MIN_PID=" + lastPid);
        }
        assertEquals(0, builder.startCheckpoint().waitFor(), "Process exited abnormally.");
    }

    @Override
    public void exec() throws Exception {
        System.out.println("Current PID = " + ProcessHandle.current().pid());
        assertLessThan(expectedLastPid, ProcessHandle.current().pid());
    }
}
