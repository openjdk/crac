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
import jdk.test.lib.Container;
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
 * @run driver jdk.test.lib.crac.CracTest      0 true
 * @run driver jdk.test.lib.crac.CracTest  86400 true
 * @run driver jdk.test.lib.crac.CracTest -86400 true
 * @run driver jdk.test.lib.crac.CracTest  86400 false
 * @run driver jdk.test.lib.crac.CracTest -86400 false
 */
public class NanoTimeTest implements CracTest {
    private static final String imageName = Common.imageName("system-nanotime");

    @CracTestArg(0)
    long monotonicOffset;

    @CracTestArg(1)
    boolean changeBootId;

    @Override
    public void test() throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            return;
        }
        CracBuilder builder = new CracBuilder();
        Path bootIdFile = Files.createTempFile("NanoTimeTest-", "-boot_id");
        try {
            // TODO: use more official image
            builder.withBaseImage("ghcr.io/crac/test-base", "latest")
                    .dockerOptions("-v", bootIdFile + ":/fake_boot_id")
                    .inDockerImage(imageName);

            Files.writeString(bootIdFile, "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\n");
            // We need to preload the library before checkpoint
            builder.doCheckpoint(Container.ENGINE_COMMAND, "exec",
                    "-e", "LD_PRELOAD=/opt/path-mapping-quiet.so",
                    "-e", "PATH_MAPPING=/proc/sys/kernel/random/boot_id:/fake_boot_id",
                    CracBuilder.CONTAINER_NAME,
                    // In case we are trying to use negative monotonic offset we could
                    // run into situation where we'd set it to negative value (prohibited).
                    // Therefore, we'll rather offset it to the future before checkpoint
                    // and set to 0 for restore.
                    "unshare", "--fork", "--time", "--monotonic", String.valueOf(Math.max(-monotonicOffset, 0)),
                    CracBuilder.DOCKER_JAVA);

            if (changeBootId) {
                Files.writeString(bootIdFile, "yyyyyyyy-yyyy-yyyy-yyyy-yyyyyyyyyyyy\n");
            }

            builder.doRestore(Container.ENGINE_COMMAND, "exec", CracBuilder.CONTAINER_NAME,
                    "unshare", "--fork", "--time", "--boottime", "86400", "--monotonic", String.valueOf(Math.max(monotonicOffset, 0)),
                    CracBuilder.DOCKER_JAVA);
        } finally {
            builder.ensureContainerKilled();
            assertTrue(bootIdFile.toFile().delete());
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
        if (changeBootId || monotonicOffset <= 0) {
            // Even though we have shifted the monotic offset by a day the difference
            // is adjusted by difference between wall clock time before and after;
            // the difference in monotonic time is considered "random"
            assertLT(after, before + TimeUnit.HOURS.toNanos(1), "After too late");
        } else {
            assertGT(after, before + TimeUnit.HOURS.toNanos(1), "After too early");
            assertLT(after, before + TimeUnit.HOURS.toNanos(25), "After too late");
        }
        long boottimeAfter = readSystemUptime();
        assertGTE(boottimeAfter, boottimeBefore + 86_400_000, "Boottime was not changed");
        RuntimeMXBean runtimeMX = ManagementFactory.getRuntimeMXBean();
        assertGTE(runtimeMX.getUptime(), 0L, "VM Uptime is negative!");
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
