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

import jdk.crac.CheckpointException;
import jdk.crac.RestoreException;
import jdk.test.lib.JDKToolFinder;
import jdk.test.lib.Utils;
import jdk.test.lib.process.ProcessTools;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import java.util.stream.Collectors;

/**
 * @test
 * @library /test/lib
 * @build CheckpointRestore
 * @run main FileDescriptorsCloseTest
 */
public class FileDescriptorsCloseTest {
    private static final String EXTRA_FD_WRAPPER = Path.of(Utils.TEST_SRC, "extra_fd_wrapper.sh").toString();

    public static void main(String[] args) throws Throwable {
        testCheckpointWithOpenFds();
        testIgnoredFileDescriptors();
    }

    private static void testCheckpointWithOpenFds() throws Throwable {
        List<String> cmd = new ArrayList<>();
        cmd.add(EXTRA_FD_WRAPPER);
        cmd.add(JDKToolFinder.getJDKTool("java"));
        cmd.add("-cp");
        cmd.add(System.getProperty("java.class.path"));
        cmd.add("-XX:CRaCCheckpointTo=./cr");
        cmd.add(CheckpointRestore.class.getSimpleName());
        // Note that the process is killed after checkpoint
        ProcessTools.executeProcess(cmd.toArray(new String[0]))
                .shouldHaveExitValue(137);

        ProcessTools.executeTestJvm("-XX:CRaCRestoreFrom=./cr")
                .shouldHaveExitValue(0)
                .shouldContain(CheckpointRestore.RESTORED_MESSAGE);
    }

    private static void testIgnoredFileDescriptors() throws Throwable {
        List<String> cmd = new ArrayList<>();
        cmd.add(EXTRA_FD_WRAPPER);
        cmd.addAll(Arrays.asList("-o", "43", "/dev/stdout"));
        cmd.addAll(Arrays.asList("-o", "45", "/dev/urandom"));
        cmd.add(JDKToolFinder.getJDKTool("java"));
        cmd.add("-cp");
        cmd.add(System.getProperty("java.class.path"));
        cmd.add("-XX:CRaCCheckpointTo=./cr");
        cmd.add("-XX:CRIgnoredFileDescriptors=43,/dev/null,44,/dev/urandom");
        cmd.add("FileDescriptorsCloseTest$TestIgnoredDescriptors");
        // Note that the process is killed after checkpoint
        ProcessTools.executeProcess(cmd.toArray(new String[0]))
                .shouldHaveExitValue(137);

        ProcessTools.executeTestJvm("-XX:CRaCRestoreFrom=./cr")
                .shouldHaveExitValue(0)
                .shouldContain(CheckpointRestore.RESTORED_MESSAGE);
    }

    public static class TestIgnoredDescriptors {
        public static void main(String[] args) throws IOException, RestoreException, CheckpointException {
            try (var stream = Files.list(Path.of("/proc/self/fd"))) {
                Map<Integer, String> fds = stream.filter(Files::isSymbolicLink)
                        .collect(Collectors.toMap(
                                f -> Integer.parseInt(f.toFile().getName()),
                                f -> {
                                    try {
                                        return Files.readSymbolicLink(f).toFile().getAbsoluteFile().toString();
                                    } catch (IOException e) {
                                        throw new RuntimeException(e);
                                    }
                                }));
                if (fds.containsKey(42)) {
                    throw new IllegalStateException("Oh no, 42 was not supposed to be ignored");
                } else if (!fds.containsKey(0) || !fds.containsKey(1) || !fds.containsKey(2)) {
                    throw new IllegalStateException("Missing standard I/O? Available: " + fds);
                } else if (!fds.containsKey(43)) {
                    throw new IllegalStateException("Missing FD 43");
                } else if (!fds.containsValue("/dev/urandom")) {
                    throw new IllegalStateException("Missing /dev/urandom");
                }
            }
            CheckpointRestore.main(args);
        }
    }
}
