/*
 * Copyright (c) 2023, 2025, Azul Systems, Inc. All rights reserved.
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
import jdk.test.lib.Utils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

import static jdk.test.lib.Asserts.*;

/**
 * @test
 * @library /test/lib
 * @build VMOptionsTest
 * @run driver jdk.test.lib.crac.CracTest
 * @requires (os.family == "linux")
 */
public class VMOptionsTest implements CracTest {
    private static final String RESTORE_MSG = "RESTORED";

    @Override
    public void test() throws Exception {
        final var enginePath = Path.of(Utils.TEST_JDK, "lib", "criuengine").toString();

        CracBuilder builder = new CracBuilder().captureOutput(true);
        builder.vmOption("-XX:CRaCEngine=criuengine");
        builder.vmOption("-XX:CRaCEngineOptions=args=-v1");
        builder.vmOption("-XX:NativeMemoryTracking=off");
        builder.doCheckpoint();

        // 1) Only restore-settable options => should succeed
        builder.clearVmOptions();
        builder.vmOption("-XX:CRaCEngine=" + enginePath);
        builder.vmOption("-XX:CRaCEngineOptions=args=-v2");
        builder.vmOption("-XX:CRaCCheckpointTo=another");
        builder.vmOption("-XX:CRaCIgnoredFileDescriptors=42,43");
        builder.vmOption("-XX:+UnlockExperimentalVMOptions");
        checkRestoreOutput(builder.doRestore());

        // 2) Adding a non-restore-settable option => should fail
        builder.vmOption("-XX:NativeMemoryTracking=summary");
        assertEquals(1, builder.startRestore().waitFor());

        // 3) Non-restore-settable option from before + allowing restore to fail => should succeed
        builder.vmOption("-XX:+CRaCIgnoreRestoreIfUnavailable");
        checkRestoreOutput(builder.doRestore());

        // 4) Only restore-settable options one of which is aliased => should succeed
        // TODO: once we have aliased restore-settable boolean options include them here
        builder.clearVmOptions();
        builder.vmOption("-XX:CREngine=" + enginePath); // Deprecated alias
        builder.vmOption("-XX:CRaCEngineOptions=args=-v2");
        builder.vmOption("-XX:CRaCCheckpointTo=another");
        builder.vmOption("-XX:CRaCIgnoredFileDescriptors=42,43");
        builder.vmOption("-XX:+UnlockExperimentalVMOptions");
        checkRestoreOutput(builder.doRestore());

        // 5) Same as (1) but options come from a settings file => should succeed
        builder.clearVmOptions();
        builder.vmOption("-XX:Flags=" + createSettingsFile(enginePath));
        checkRestoreOutput(builder.doRestore());

        // 6) Same as (1) but options come from a VM options file => should succeed
        builder.clearVmOptions();
        builder.vmOption("-XX:VMOptionsFile=" + createVMOptionsFile(enginePath));
        checkRestoreOutput(builder.doRestore());
    }

    @Override
    public void exec() throws RestoreException, CheckpointException {
        {
            HotSpotDiagnosticMXBean bean = ManagementFactory.getPlatformMXBean(HotSpotDiagnosticMXBean.class);

            VMOption engine = bean.getVMOption("CRaCEngine");
            assertEquals("criuengine", engine.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, engine.getOrigin());

            VMOption engineOptions = bean.getVMOption("CRaCEngineOptions");
            assertEquals("args=-v1", engineOptions.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, engineOptions.getOrigin());

            VMOption checkpointTo = bean.getVMOption("CRaCCheckpointTo");
            assertEquals("cr", checkpointTo.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, checkpointTo.getOrigin());

            VMOption nmt = bean.getVMOption("NativeMemoryTracking");
            assertEquals("off", nmt.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, nmt.getOrigin());

            VMOption restoreFrom = bean.getVMOption("CRaCRestoreFrom");
            assertEquals("", restoreFrom.getValue());
            assertEquals(VMOption.Origin.DEFAULT, restoreFrom.getOrigin());

            VMOption unlockExperimentalOpts = bean.getVMOption("UnlockExperimentalVMOptions");
            assertEquals("false", unlockExperimentalOpts.getValue());
            assertEquals(VMOption.Origin.DEFAULT, unlockExperimentalOpts.getOrigin());
        }

        Core.checkpointRestore();
        System.out.println(RESTORE_MSG);

        {
            HotSpotDiagnosticMXBean bean = ManagementFactory.getPlatformMXBean(HotSpotDiagnosticMXBean.class);

            // Should not change

            VMOption engine = bean.getVMOption("CRaCEngine");
            assertEquals("criuengine", engine.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, engine.getOrigin());

            VMOption engineOptions = bean.getVMOption("CRaCEngineOptions");
            assertEquals("args=-v1", engineOptions.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, engineOptions.getOrigin());

            VMOption nmt = bean.getVMOption("NativeMemoryTracking");
            assertEquals("off", nmt.getValue());
            assertEquals(VMOption.Origin.VM_CREATION, nmt.getOrigin());

            // Should change

            VMOption checkpointTo = bean.getVMOption("CRaCCheckpointTo");
            assertEquals("another", checkpointTo.getValue());
            assertEquals(VMOption.Origin.OTHER, checkpointTo.getOrigin());

            VMOption restoreFrom = bean.getVMOption("CRaCRestoreFrom");
            assertEquals("cr", restoreFrom.getValue());
            assertEquals(VMOption.Origin.OTHER, restoreFrom.getOrigin());

            VMOption ignoredFileDescriptors = bean.getVMOption("CRaCIgnoredFileDescriptors");
            assertEquals("42,43", ignoredFileDescriptors.getValue());
            assertEquals(VMOption.Origin.OTHER, ignoredFileDescriptors.getOrigin());

            VMOption unlockExperimentalOpts = bean.getVMOption("UnlockExperimentalVMOptions");
            assertEquals("true", unlockExperimentalOpts.getValue());
            assertEquals(VMOption.Origin.OTHER, unlockExperimentalOpts.getOrigin());
        }
    }

    private static void checkRestoreOutput(CracProcess restored) throws Exception {
        restored.outputAnalyzer()
            .shouldNotContain("[warning]")
            .shouldNotContain("[error]")
            .stdoutShouldContain(RESTORE_MSG);
    }

    private static String createSettingsFile(String enginePath) throws Exception {
        final var path = Utils.createTempFile("settings", ".txt");
        Files.write(path, List.of(
            "CRaCEngine=" + enginePath,
            "CRaCEngineOptions=args=-v2",
            "CRaCCheckpointTo=another",
            "CRaCIgnoredFileDescriptors=42,43",
            "+UnlockExperimentalVMOptions"
        ));
        return path.toString();
    }

    private static String createVMOptionsFile(String enginePath) throws Exception {
        final var path = Utils.createTempFile("vmoptions", ".txt");
        Files.write(path, List.of(
            "-XX:CRaCEngine=" + enginePath,
            "-XX:CRaCEngineOptions=args=-v2",
            "-XX:CRaCCheckpointTo=another",
            "-XX:CRaCIgnoredFileDescriptors=42,43",
            "-XX:+UnlockExperimentalVMOptions"
        ));
        return path.toString();
    }
}
