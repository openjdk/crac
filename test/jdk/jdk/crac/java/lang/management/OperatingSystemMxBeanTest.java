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

import com.sun.management.OperatingSystemMXBean;
import jdk.crac.Core;
import jdk.test.lib.crac.*;

import java.lang.management.ManagementFactory;

import static jdk.test.lib.Asserts.*;

/*
 * @test
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build OperatingSystemMxBeanTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class OperatingSystemMxBeanTest implements CracTest {
    @Override
    public void test() throws Exception {
        // The restore must be in a new process => cannot use simengine
        new CracBuilder().engine(CracEngine.CRIU).doCheckpointAndRestore();
    }

    @Override
    public void exec() throws Exception {
        OperatingSystemMXBean bean = ManagementFactory.getPlatformMXBean(OperatingSystemMXBean.class);
        System.out.println("System CPU load:  " + bean.getCpuLoad());
        System.out.println("Process CPU load: " + bean.getProcessCpuLoad());
        Core.checkpointRestore();
        // We're restoring on the same system, so total CPU load should not have failed
        assertLTE(0.0, bean.getCpuLoad());
        // Per process ticks should be lower after restore than before checkpoint => load unavailable
        assertEquals(-1.0, bean.getProcessCpuLoad());
        // Second invocation should work with the updated value => correct
        assertLTE(0.0, bean.getProcessCpuLoad());
    }
}
