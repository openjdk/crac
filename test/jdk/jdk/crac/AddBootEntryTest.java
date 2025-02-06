/*
 * Copyright (c) 2024, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.Core;
import jdk.test.lib.Utils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.io.File;
import java.io.IOException;
import java.lang.instrument.Instrumentation;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.jar.JarFile;

/**
 * @test AddBootEntryTest
 * @requires (os.family == "linux")
 * @library /test/lib
 * @modules java.instrument
 *          jdk.jartool/sun.tools.jar
 * @build AddBootEntryTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class AddBootEntryTest implements CracTest {
    @Override
    public void test() throws Exception {
        File agentJar = File.createTempFile("agent", ".jar");
        agentJar.deleteOnExit();

        File manifest = File.createTempFile("manifest", ".mf");
        manifest.deleteOnExit();
        Files.writeString(manifest.toPath(), """
            Manifest-Version: 1.0
            Premain-Class: AddBootEntryTest$Agent
            Can-Redefine-Classes: true
            """);

        File testJar = File.createTempFile("test", ".jar");
        testJar.deleteOnExit();

        String[] agentJarArgs = { "--create", "--file", agentJar.getPath(),
                "--manifest", manifest.getAbsolutePath(),
                Utils.TEST_CLASSES + File.separator + "AddBootEntryTest$Agent.class" };
        sun.tools.jar.Main jarTool = new sun.tools.jar.Main(System.out, System.err, "jar");
        if (!jarTool.run(agentJarArgs)) {
            throw new Exception("jar failed: args=" + Arrays.toString(agentJarArgs));
        }
        String[] testJarArgs = {"--create", "--file", testJar.getPath(),
                Utils.TEST_CLASSES + File.separator + "AddBootEntryTest$Agent.class"};
        if (!jarTool.run(testJarArgs)) {
            throw new Exception("jar failed: args=" + Arrays.toString(testJarArgs));
        }

        new CracBuilder()
                .vmOption("-javaagent:" + agentJar.getPath() + "=" + testJar.getPath())
                .printResources(true)
                .doCheckpoint();
        new CracBuilder().doRestore();
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
    }

    public static class Agent {
        public static void premain(String args, Instrumentation inst) throws IOException {
            inst.appendToBootstrapClassLoaderSearch(new JarFile(args));
        }
    }

    public static class Dummy {}
}
