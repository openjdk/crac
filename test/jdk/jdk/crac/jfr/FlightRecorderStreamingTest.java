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
import jdk.jfr.consumer.RecordingStream;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.time.Duration;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/**
 * @test FlightRecorderStreamingTest
 * @library /test/lib
 * @requires (os.family == "linux")
 * @build FlightRecorderStreamingTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class FlightRecorderStreamingTest implements CracTest {
    public static final String JDK_CPULOAD = "jdk.CPULoad";

    @Override
    public void test() throws Exception {
        new CracBuilder().doCheckpointAndRestore();
    }

    @Override
    public void exec() throws Exception {
        AtomicReference<CountDownLatch> latch = new AtomicReference<>(new CountDownLatch(1));
        try (var rs = new RecordingStream()) {
            rs.enable(JDK_CPULOAD).withPeriod(Duration.ofMillis(1));
            rs.onEvent(JDK_CPULOAD, _ -> latch.get().countDown());
            rs.startAsync();
            // Wait until we receive at least one event
            latch.get().await();
            CRaCMXBean.getCRaCMXBean().checkpointRestore();
            // Wait for some new event to arrive
            latch.set(new CountDownLatch(1));
            latch.get().await();
        }
    }
}
