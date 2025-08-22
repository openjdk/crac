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
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import java.util.stream.Collectors;

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

    private record VMOptionSpec(
        String name,
        String strValue,
        Boolean boolValue,
        /**
         * Whether the option can be changed in the restored JVM. This is not
         * the same as being RESTORE_SETTABLE: for this to be true the option
         * must be RESTORE_SETTABLE but some RESTORE_SETTABLE options are not
         * applied in the restored JVM (e.g. engine options).
         */
        boolean canChangeOnRestore
    ) {
        public VMOptionSpec {
            assertNotNull(name, "Option must have a name");
            assertTrue(strValue != null ^ boolValue != null, "Option must have one type of value");
        }

        public static VMOptionSpec ofStr(String name, String value, boolean canChangeOnRestore) {
            assertNotNull(value, "String option must have a value");
            return new VMOptionSpec(name, value, null, canChangeOnRestore);
        }

        public static VMOptionSpec ofBool(String name, boolean value, boolean canChangeOnRestore) {
            return new VMOptionSpec(name, null, value, canChangeOnRestore);
        }

        public boolean isStr() {
            return strValue != null;
        }

        public boolean isBool() {
            return boolValue != null;
        }

        public String asArgument() {
            return isStr() ? name + "=" + strValue : (boolValue ? "+" : "-") + name;
        }

        public String valueAsString() {
            return isStr() ? strValue : boolValue.toString();
        }
    };

    private static final List<VMOptionSpec> OPTIONS_CHECKPOINT = List.of(
        VMOptionSpec.ofStr("CRaCEngine", "criu", false),
        VMOptionSpec.ofStr("CRaCEngineOptions", "args=-v1", false),
        VMOptionSpec.ofStr("CRaCCheckpointTo", new CracBuilder().imageDir().toString(), true),
        VMOptionSpec.ofStr("NativeMemoryTracking", "off", false)
    );
    private static final List<VMOptionSpec> OPTIONS_RESTORE = List.of(
        VMOptionSpec.ofStr("CRaCEngine", "criuengine", false),
        VMOptionSpec.ofStr("CRaCEngineOptions", "args=-v2", false),
        VMOptionSpec.ofStr("CRaCCheckpointTo", "another", true),
        VMOptionSpec.ofStr("CRaCIgnoredFileDescriptors", "42,43", true),
        VMOptionSpec.ofBool("UnlockExperimentalVMOptions", true, true)
    );

    @Override
    public void test() throws Exception {
        final var builder = new CracBuilder().engine(CracEngine.CRIU).captureOutput(true);
        setVmOptions(builder, OPTIONS_CHECKPOINT);
        builder.doCheckpoint();

        // Only restore-settable options => should succeed
        builder.clearVmOptions();
        setVmOptions(builder, OPTIONS_RESTORE);
        checkRestoreOutput(builder.doRestore());

        // Adding a non-restore-settable option => should fail
        builder.vmOption("-XX:NativeMemoryTracking=summary");
        builder.startRestore().outputAnalyzer().shouldHaveExitValue(1).stderrShouldContain(
            "VM option 'NativeMemoryTracking' is not restore-settable and is not available on restore"
        );

        // Non-restore-settable option from before + allowing restore to fail => should succeed
        builder.vmOption("-XX:+CRaCIgnoreRestoreIfUnavailable");
        checkRestoreOutput(builder.doRestore());

        // Only restore-settable options one of which is aliased => should succeed
        // TODO: once we have aliased restore-settable boolean options include them here
        builder.clearVmOptions();
        setVmOptions(builder, OPTIONS_RESTORE);
        builder.vmOption("-XX:CREngine=criuengine"); // Deprecated alias
        checkRestoreOutput(builder.doRestore());

        // Only restore-settable options coming from a settings file => should succeed
        builder.clearVmOptions();
        builder.vmOption("-XX:Flags=" + createSettingsFile(OPTIONS_RESTORE));
        checkRestoreOutput(builder.doRestore());

        // Only restore-settable options coming from a VM options file => should succeed
        builder.clearVmOptions();
        builder.vmOption("-XX:VMOptionsFile=" + createVMOptionsFile(OPTIONS_RESTORE));
        checkRestoreOutput(builder.doRestore());

        // Unrecognized option => should fail
        builder.clearVmOptions();
        setVmOptions(builder, OPTIONS_RESTORE);
        builder.vmOption("-XX:SomeNonExistentOption=abc");
        builder.startRestore().outputAnalyzer().shouldHaveExitValue(1).stderrShouldContain(
            "Unrecognized VM option 'SomeNonExistentOption=abc'"
        );

        // Unrecognized option from before + IgnoreUnrecognizedVMOptions => should succeed
        builder.vmOption("-XX:+IgnoreUnrecognizedVMOptions");
        checkRestoreOutput(builder.doRestore());
    }

    @Override
    public void exec() throws RestoreException, CheckpointException {
        HotSpotDiagnosticMXBean bean = ManagementFactory.getPlatformMXBean(HotSpotDiagnosticMXBean.class);

        for (final var optSpec : OPTIONS_CHECKPOINT) {
            final var opt = bean.getVMOption(optSpec.name());
            assertEquals(optSpec.valueAsString(), opt.getValue(), optSpec.name() + ": value not set before checkpoint");
            assertEquals(VMOption.Origin.VM_CREATION, opt.getOrigin(), optSpec.name() + ": unexpected origin before checkpoint");
        }
        for (final var optSpec : OPTIONS_RESTORE) {
            final var opt = bean.getVMOption(optSpec.name());
            final var expectedOrigin = OPTIONS_CHECKPOINT.stream().anyMatch(o -> o.name().equals(optSpec.name())) ?
                VMOption.Origin.VM_CREATION : VMOption.Origin.DEFAULT;
            assertEquals(expectedOrigin, opt.getOrigin(), optSpec.name() + ": unexpected origin before checkpoint");
        }

        Core.checkpointRestore();
        System.out.println(RESTORE_MSG);

        for (final var optSpec : OPTIONS_CHECKPOINT) {
            if (!optSpec.canChangeOnRestore() || OPTIONS_RESTORE.stream().noneMatch(o -> o.name().equals(optSpec.name()))) {
                final var opt = bean.getVMOption(optSpec.name());
                assertEquals(optSpec.valueAsString(), opt.getValue(), optSpec.name() + ": value changed after restore");
                assertEquals(VMOption.Origin.VM_CREATION, opt.getOrigin(), optSpec.name() + ": origin changed after restore");
            }
        }
        {
            final var ignoreUnrecognized = bean.getVMOption("IgnoreUnrecognizedVMOptions");
            assertEquals(VMOption.Origin.DEFAULT, ignoreUnrecognized.getOrigin(), "IgnoreUnrecognizedVMOptions: origin changed after restore");
        }
        for (final var optSpec : OPTIONS_RESTORE) {
            final var opt = bean.getVMOption(optSpec.name());
            if (!optSpec.canChangeOnRestore()) {
                final var expectedOrigin = OPTIONS_CHECKPOINT.stream().anyMatch(o -> o.name().equals(optSpec.name())) ?
                    VMOption.Origin.VM_CREATION : VMOption.Origin.DEFAULT;
                assertEquals(expectedOrigin, opt.getOrigin(), optSpec.name() + ": origin changed after restore");
            } else {
                assertEquals(optSpec.valueAsString(), opt.getValue(), optSpec.name() + ": value not changed after restore");
                assertEquals(VMOption.Origin.OTHER, opt.getOrigin(), optSpec.name() + ": unexpected origin after restore");
            }
        }
    }

    private static void checkRestoreOutput(CracProcess restored) throws Exception {
        restored.outputAnalyzer()
            .shouldNotContain("[warning]")
            .shouldNotContain("[error]")
            .stdoutShouldContain(RESTORE_MSG);
    }

    private static void setVmOptions(CracBuilder builder, List<VMOptionSpec> options) {
        for (final var opt : options) {
            builder.vmOption("-XX:" + opt.asArgument());
        }
    }

    private static String createSettingsFile(List<VMOptionSpec> options) throws Exception {
        final var path = Utils.createTempFile("settings", ".txt");
        Files.write(
            path,
            options.stream().map(opt -> opt.asArgument()).collect(Collectors.toList())
        );
        return path.toString();
    }

    private static String createVMOptionsFile(List<VMOptionSpec> options) throws Exception {
        final var path = Utils.createTempFile("vmoptions", ".txt");
        Files.write(
            path,
            options.stream().map(opt -> "-XX:" + opt.asArgument()).collect(Collectors.toList())
        );
        return path.toString();
    }
}
