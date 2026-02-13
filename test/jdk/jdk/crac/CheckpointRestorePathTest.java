/*
 * Copyright (c) 2022, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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

import jdk.crac.Core;
import jdk.test.lib.Platform;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;
import jdk.test.lib.util.FileUtils;

import java.io.File;
import java.lang.reflect.Method;
import java.nio.file.Path;
import java.util.concurrent.Callable;

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertTrue;

/**
 * @test id=CHECKPOINT
 * @library /test/lib
 * @build CheckpointRestorePathTest
 * @run driver jdk.test.lib.crac.CracTest testEmpty
 * @run driver jdk.test.lib.crac.CracTest testNone
 * @run driver jdk.test.lib.crac.CracTest testDeep
 */
/**
 * @test id=RESTORE
 * @library /test/lib
 * @requires (os.family == "linux")
 * @build CheckpointRestorePathTest
 * @run driver jdk.test.lib.crac.CracTest testRestoreEmpty
 * @run driver jdk.test.lib.crac.CracTest testRestoreNoDir
 * @run driver jdk.test.lib.crac.CracTest testRestoreNoImage
 * @run driver jdk.test.lib.crac.CracTest testRestoreNoImageSkipCpuFeaturesCheck
 */
public class CheckpointRestorePathTest implements CracTest {
    static final String DEEPLY_NESTED_CR = "deeply/nested/cr";
    static final String NON_EXISTENT_CR = "non/existent/cr";

    @CracTestArg
    Method variant;

    @Override
    public void test() throws Exception {
        variant.invoke(this);
    }

    void testEmpty() throws Exception {
        checkNotConfigured(new CracBuilder().engine(CracEngine.SIMULATE).imageDir(""));
    }

    void testNone() throws Exception {
        checkNotConfigured(new CracBuilder().engine(CracEngine.SIMULATE).imageDir(null));
    }

    void checkNotConfigured(CracBuilder builder) throws Exception {
        builder.captureOutput(true)
                .startCheckpoint().outputAnalyzer()
                .shouldHaveExitValue(1)
                .stderrShouldContain("C/R is not configured");
    }

    void testDeep() throws Exception {
        new CracBuilder().engine(CracEngine.SIMULATE).imageDir(DEEPLY_NESTED_CR).captureOutput(true)
                .startCheckpoint().outputAnalyzer()
                .shouldHaveExitValue(1)
                // unified logging going to standard output rather than stderr by default
                .stdoutShouldContain("Cannot create CRaCCheckpointTo=" + DEEPLY_NESTED_CR);
    }

    void testRestoreEmpty() throws Exception {
        // Empty CRaCRestoreFrom should result in default java usage output
        // as if the VM option was missing (we won't test that case)
        new CracBuilder().engine(CracEngine.PAUSE).imageDir("").captureOutput(true)
                .startRestore().outputAnalyzer()
                .shouldHaveExitValue(1)
                .stderrShouldContain("Usage: java");
    }

    void testRestoreNoDir() throws Exception {
        new CracBuilder().engine(CracEngine.PAUSE).imageDir(NON_EXISTENT_CR).captureOutput(true)
                .startRestore().outputAnalyzer()
                .shouldHaveExitValue(1)
                .stdoutShouldContain("Cannot open CRaCRestoreFrom=" + NON_EXISTENT_CR)
                .stdoutShouldContain("Failed to restore from " + NON_EXISTENT_CR);
    }

    void testRestoreNoImage() throws Exception {
        // TODO: make the engine check pidfile before tags to have the same message regardless of CPU features check
        testRestoreNoImage(false, Platform.isX64() ? "cannot open cr/tags" : "fopen pidfile: No such file or directory");
    }

    void testRestoreNoImageSkipCpuFeaturesCheck() throws Exception {
        testRestoreNoImage(true, "simengine: fopen pidfile: No such file or directory");
    }

    void testRestoreNoImage(boolean skipCpuFeatures, String errMessage) throws Exception {
        CracBuilder builder = new CracBuilder().engine(CracEngine.PAUSE);
        if (builder.imageDir().toFile().exists()) {
            FileUtils.deleteFileTreeWithRetry(builder.imageDir());
        }
        assertTrue(new File("cr").mkdir());
        if (skipCpuFeatures) {
            builder.vmOption("-XX:+UnlockExperimentalVMOptions").vmOption("-XX:CheckCPUFeatures=skip");
        }
        builder.captureOutput(true).startRestore().outputAnalyzer()
                .shouldHaveExitValue(1)
                .stderrShouldContain(errMessage);
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
    }
}
