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
import jdk.test.lib.process.OutputAnalyzer;

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertLessThan;

import java.util.Arrays;

/*
 * @test ContainerPidAdjustmentTest
 * @summary The test checks that process PID is adjusted with the specified value, when checkpointing in a container. Default min PID value is 128.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build ContainerPidAdjustmentTest
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   1       true   1
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   100     true   100
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   1000    false  1000
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   2000    true   2000   1000
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   0       true   -1
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   -10     true   -1
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   blabla  true   -1
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   5000000 true   -1
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   5000000 true   -1     4194200
 * @run driver/timeout=60 jdk.test.lib.crac.CracTest   4194303 true   -1
 */
public class ContainerPidAdjustmentTest implements CracTest {
    @CracTestArg(0)
    String lastPid;

    @CracTestArg(1)
    boolean usePrivilegedContainer;

    @CracTestArg(2)
    long expectedLastPid;

    @CracTestArg(value = 3, optional = true)
    String lastPidSetup;

    final private String CURRENT_PID_MESSAGE = "Current PID = ";

    @Override
    public void test() throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            return;
        }
        CracBuilder builder = new CracBuilder()
            .inDockerImage("pid-adjustment")
            .vmOption("-XX:CRaCMinPid=" + lastPid)
            .containerUsePrivileged(usePrivilegedContainer);
        if (0 > expectedLastPid) {
            builder.captureOutput(true);
        }
        if (null != lastPidSetup) {
            // Set up the initial last pid,
            // create a non-privileged user,
            // and force spinning the last pid running checkpoint under the user.
            builder
                .containerSetup(Arrays.asList("bash", "-c", "useradd the_user && echo " + lastPidSetup + " >/proc/sys/kernel/ns_last_pid"))
                .dockerCheckpointOptions(Arrays.asList("-u", "the_user"));
        }

        if (0 < expectedLastPid) {
            builder.startCheckpoint().waitForSuccess();
        } else {
            final int expectedExitValue = (int)java.lang.Math.abs(expectedLastPid);
            CracProcess process = builder.startCheckpoint();
            final int exitValue = process.waitFor();
            assertEquals(expectedExitValue, exitValue, "Process returned unexpected exit code: " + exitValue);
            OutputAnalyzer oa = process.outputAnalyzer();
            oa.shouldNotContain(CURRENT_PID_MESSAGE);
            if (null != lastPidSetup) {
                oa.shouldContain("spin_last_pid: Invalid argument (" + lastPid + ")");
            }
        }
    }

    @Override
    public void exec() throws Exception {
        System.out.println(CURRENT_PID_MESSAGE + ProcessHandle.current().pid());
        assertLessThan((long)0, expectedLastPid, "Shouldn't happen");
        assertLessThan(expectedLastPid, ProcessHandle.current().pid());
    }
}
