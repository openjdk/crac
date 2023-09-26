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

import jdk.crac.Core;
import jdk.test.lib.Utils;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.file.Path;
import java.util.*;
import java.util.stream.Collectors;

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertTrue;

/**
 * This test includes two behaviours:
 * 1) inheriting open FD from parent process: this is achieved using EXTRA_FD_WRAPPER
 *    - any excess inherited FDs should be closed when JVM starts.
 * 2) open files on classpath: these files are ignored, handling is left to CREngine
 *
 * @test
 * @library /test/lib
 * @build CheckpointWithOpenFdsTest
 * @run driver jdk.test.lib.crac.CracTest
 * @requires (os.family == "linux")
 */
public class CheckpointWithOpenFdsTest implements CracTest {
    private static final String EXTRA_FD_WRAPPER = Path.of(Utils.TEST_SRC, "extra_fd_wrapper.sh").toString();

    @CracTestArg(optional = true, value = 0)
    String relativePathToSomeJar;

    @CracTestArg(optional = true, value = 1)
    String absolutePathToSomeJar;

    @Override
    public void test() throws Exception {
        List<Path> jars = Arrays.stream(System.getProperty("java.class.path").split(File.pathSeparator))
                .filter(p -> p.endsWith(".jar")).map(Path::of).toList();
        assertEquals(2, jars.size()); // usually we have javatest.jar and jtreg.jar
        assertTrue(jars.stream().allMatch(jar -> jar.toFile().exists()));
        Path firstJar = jars.get(0);
        Path secondJar = jars.get(1);
        assertTrue(secondJar.isAbsolute());
        Path cwd = Path.of(System.getProperty("user.dir"));
        String relative = cwd.relativize(firstJar).toString();
        String absolute = secondJar.toString();

        CracBuilder builder = new CracBuilder();
        builder.classpathEntry(relative).classpathEntry(absolute).args(CracTest.args(relative, absolute));
        builder.startCheckpoint(Arrays.asList(EXTRA_FD_WRAPPER, CracBuilder.JAVA)).waitForCheckpointed();
        builder.captureOutput(true).doRestore().outputAnalyzer().shouldContain(RESTORED_MESSAGE);
    }

    @Override
    public void exec() throws Exception {
        String absolute = Path.of(relativePathToSomeJar).toAbsolutePath().toString();

        Path cwd = Path.of(System.getProperty("user.dir"));
        String relative = cwd.relativize(Path.of(absolutePathToSomeJar)).toString();
        try (var file1 = new RandomAccessFile(absolute, "r");
             var file2 = new RandomAccessFile(relative, "r")) {
            Core.checkpointRestore();
            System.out.println(RESTORED_MESSAGE);
        }
    }
}
