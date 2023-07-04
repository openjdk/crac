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

import com.sun.management.HotSpotDiagnosticMXBean;
import com.sun.management.VMOption;
import jdk.crac.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;

import java.lang.management.ManagementFactory;

import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test
 * @library /test/lib
 * @build VMOptionsTest
 * @run driver jdk.test.lib.crac.CracTest
 * @requires (os.family == "linux")
 */
public class VMOptionsTest implements CracTest {
    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder();
        // this is here just to test passing CREngine params
        builder.engine(CracEngine.CRIU, "--verbosity=4", "--log-file=/dev/null");
        builder.vmOption("-XX:NativeMemoryTracking=off");
        builder.doCheckpoint();
        builder.clearVmOptions();
        builder.vmOption("-XX:CRaCCheckpointTo=another"); // manageable
        builder.vmOption("-XX:CRaCIgnoredFileDescriptors=42,43"); // restore_settable
        builder.doRestore();
        // Setting non-manageable option
        builder.vmOption("-XX:NativeMemoryTracking=summary");
        assertEquals(1, builder.startRestore().waitFor());
    }

    @Override
    public void exec() throws RestoreException, CheckpointException {
        {
            HotSpotDiagnosticMXBean bean = ManagementFactory.getPlatformMXBean(HotSpotDiagnosticMXBean.class);
            VMOption checkpointTo1 = bean.getVMOption("CRaCCheckpointTo");
            assertEquals("cr", checkpointTo1.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, checkpointTo1.getOrigin());
            VMOption nmt1 = bean.getVMOption("NativeMemoryTracking");
            assertEquals("off", nmt1.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, nmt1.getOrigin());
        }

        Core.checkpointRestore();

        {
            HotSpotDiagnosticMXBean bean = ManagementFactory.getPlatformMXBean(HotSpotDiagnosticMXBean.class);
            VMOption checkpointTo2 = bean.getVMOption("CRaCCheckpointTo");
            assertEquals("another", checkpointTo2.getValue());
            assertEquals(VMOption.Origin.OTHER, checkpointTo2.getOrigin());
            VMOption ignoredFileDescriptors = bean.getVMOption("CRaCIgnoredFileDescriptors");
            assertEquals("42,43", ignoredFileDescriptors.getValue());
            assertEquals(VMOption.Origin.OTHER, ignoredFileDescriptors.getOrigin());
            VMOption nmt = bean.getVMOption("NativeMemoryTracking");
            assertEquals("off", nmt.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, nmt.getOrigin());
        }
    }
}
