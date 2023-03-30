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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import jdk.crac.*;
import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.IOException;
import java.lang.management.ManagementFactory;
import java.lang.management.RuntimeMXBean;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.concurrent.TimeUnit;

import static jdk.test.lib.Asserts.*;

/**
 * @test NanoTimeTest
 * @requires (os.family == "linux")
 * @requires docker.support
 * @library /test/lib
 * @build NanoTimeTest
 * @run driver jdk.test.lib.crac.CracTest 0
 * @run driver jdk.test.lib.crac.CracTest 86400
 * @run driver jdk.test.lib.crac.CracTest -86400
 */
public class NanoTimeTest implements CracTest {
    private static final String imageName = Common.imageName("system-nanotime");

    @CracTestArg
    long monotonicOffset;

    @Override
    public void test() throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            return;
        }
        CracBuilder builder = new CracBuilder().inDockerImage(imageName);
        try {
            builder.doCheckpoint();

            builder.startRestore(Arrays.asList("docker", "exec", CracBuilder.CONTAINER_NAME,
                    "unshare", "--fork", "--time", "--boottime", "86400", "--monotonic", String.valueOf(monotonicOffset),
                    CracBuilder.DOCKER_JAVA)).waitForSuccess();
        } finally {
            builder.ensureContainerKilled();
        }
    }

    @Override
    public void exec() throws Exception {
        System.out.println("Expected offset: " + monotonicOffset);
        // We use uptime to assert that changing the clock worked
        long boottimeBefore = readSystemUptime();

        long before = System.nanoTime();
        Core.checkpointRestore();
        long after = System.nanoTime();
        System.out.println("Before: " + before);
        System.out.println("After: " + after);
        assertLTE(before, after, "After < Before");
        // Even though we have shifted the monotic offset by a day the difference
        // is adjusted by difference between wall clock time before and after;
        // the difference in monotonic time is considered "random"
        assertLT(after, before + TimeUnit.HOURS.toNanos(1), "After too late");

        long boottimeAfter = readSystemUptime();
        assertGTE(boottimeAfter, boottimeBefore + 86_400_000, "Boottime was not changed");
        RuntimeMXBean runtimeMX = ManagementFactory.getRuntimeMXBean();
        assertGTE(runtimeMX.getUptime(), 0L,"VM Uptime is negative!");
        CRaCMXBean cracBean = CRaCMXBean.getCRaCMXBean();
        assertLT(cracBean.getUptimeSinceRestore(), 60_000L);
        assertGTE(cracBean.getUptimeSinceRestore(), 0L);
    }

    private long readSystemUptime() throws IOException {
        String uptimeStr = Files.readString(Path.of("/proc/uptime"));
        String[] parts = uptimeStr.split(" ");
        return (long)(Double.parseDouble(parts[0]) * 1000);
    }
}
