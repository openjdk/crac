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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
import jdk.crac.management.CRaCMXBean;
import jdk.jfr.Recording;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.nio.file.Path;
import java.time.Duration;

/**
 * @test RecordingNotRunningTest
 * @library /test/lib
 * @requires (os.family == "linux")
 * @build RecordingNotRunningTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class RecordingNotRunningTest implements CracTest {
    public static final String JDK_CPULOAD = "jdk.CPULoad";

    @Override
    public void test() throws Exception {
        new CracBuilder().doCheckpointAndRestore();
    }

    @Override
    public void exec() throws Exception {
        try (Recording stopped = createAndConfigure();
             Recording notStarted = createAndConfigure();
             Recording delayed = createAndConfigure()) {
            stopped.start();
            Thread.sleep(1000);
            stopped.stop();

            delayed.scheduleStart(Duration.ofDays(1));

            CRaCMXBean.getCRaCMXBean().checkpointRestore();

            notStarted.start();
            notStarted.stop();
            delayed.start();
            delayed.stop();
        }
    }

    private static Recording createAndConfigure() throws IOException {
        Recording recording = new Recording();
        recording.enable(JDK_CPULOAD).withPeriod(Duration.ofMillis(1));
        // If the destination is not set we'd need to call close() explicitly after stop() and dump()
        recording.setDestination(Path.of("recording.jfr"));
        return recording;
    }
}
