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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package crac.fileDescriptors;

import jdk.crac.Core;
import jdk.test.lib.compiler.CompilerUtils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.process.OutputAnalyzer;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

import static jdk.test.lib.Asserts.assertTrue;
import static jdk.test.lib.Asserts.assertEquals;

/**
 * @test PatchModuleTest
 * @summary Verifies that checkpointing works correctly with --patch-module.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @modules jdk.jartool/sun.tools.jar
 * @build crac.fileDescriptors.PatchModuleTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class PatchModuleTest implements CracTest {

    // A simple new class that we will "add" to the java.lang package.
    private static final String PATCH_MESSAGE = "Patched!";
    private static final String PATCHED_CLASS_NAME = "CracPatchTester";
    private static final String PATCHED_CLASS_FULL_NAME = "java.lang." + PATCHED_CLASS_NAME;
    private static final String PATCHED_CLASS_SOURCE_FILE = PATCHED_CLASS_NAME + ".java";
    private static final String PATCHED_CLASS_SOURCE = "package java.lang; " +
            "public final class CracPatchTester { " +
            "    public static String getInfo() { return \"" + PATCH_MESSAGE + "\"; } " +
            "}";

    @Override
    public void test() throws Exception {
        // 1. Prepare source directory and the custom class source file
        Path srcDir = Files.createTempDirectory("patch-src");
        Path patchSrcDir = srcDir.resolve("java").resolve("lang");
        Files.createDirectories(patchSrcDir);
        Files.writeString(patchSrcDir.resolve(PATCHED_CLASS_SOURCE_FILE), PATCHED_CLASS_SOURCE);

        // 2. Compile the source using --patch-module to place it in java.base
        Path classesDir = Files.createTempDirectory("patch-classes");
        boolean compiled = CompilerUtils.compile(
                srcDir,
                classesDir,
                "--patch-module=java.base=" + srcDir.toString());
        assertTrue(compiled, "Compilation of patch class with --patch-module failed");

        // 3. Create the patch JAR file from the compiled class
        File patchJar = File.createTempFile("patch", ".jar");
        patchJar.deleteOnExit();

        String[] patchJarArgs = {
                "--create",
                "--file", patchJar.getAbsolutePath(),
                "-C", classesDir.toString(), "."
        };
        sun.tools.jar.Main jarTool = new sun.tools.jar.Main(System.out, System.err, "jar");
        if (!jarTool.run(patchJarArgs)) {
            throw new Exception("Failed to create patch JAR: " + Arrays.toString(patchJarArgs));
        }

        // 4. Build the process with --patch-module
        CracBuilder builder = new CracBuilder()
                .captureOutput(true)
                .printResources(true)
                .vmOption("--patch-module=java.base=" + patchJar.getAbsolutePath());

        // 5. Start the checkpoint process
        CracProcess checkpointProcess = builder.startCheckpoint();

        // 6. Analyze the output of the checkpointing process to verify fixes
        OutputAnalyzer checkpointOutput = checkpointProcess.outputAnalyzer();

        // Verify that the checkpoint process did not fail
        checkpointOutput.shouldNotContain("CheckpointException");

        // Verify Java-level fix: PersistentJarFile is used
        checkpointOutput.shouldContain(patchJar.getAbsolutePath() + " is recorded as always available on restore");

        // Verify Native-level fix: check_fds reports the FD as ok
        checkpointOutput.shouldMatch("JVM: FD fd=[0-9]+ type=regular path=\"" + patchJar.getAbsolutePath()
                + "\" OK: claimed by patch-module");

        // Let the checkpoint finish
        checkpointProcess.waitForCheckpointed();

        // 7. Perform restore
        builder.doRestore();
    }

    @Override
    public void exec() throws Exception {
        // Verify the patch is active by loading and calling the new class
        String infoBefore = invokePatchedMethod();
        assertEquals(PATCH_MESSAGE, infoBefore, "Patch should be active before checkpoint.");

        // Perform checkpoint and restore
        Core.checkpointRestore();

        // Verify the patch is still active after restore
        String infoAfter = invokePatchedMethod();
        assertEquals(PATCH_MESSAGE, infoAfter, "Patch should remain active after restore.");
    }

    private String invokePatchedMethod() throws Exception {
        Class<?> patcherClass = Class.forName(PATCHED_CLASS_FULL_NAME);
        return (String) patcherClass.getMethod("getInfo").invoke(null);
    }
}
