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
import jdk.crac.Core;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.util.List;
import java.util.stream.IntStream;

import static jdk.test.lib.Asserts.*;

/**
 * @test
 * @library /test/lib
 * @modules java.base/jdk.internal.crac:+open
 * @requires (os.family == "linux")
 * @build ProcessPipelineTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class ProcessPipelineTest implements CracTest {
    @Override
    public void test() throws Exception {
        new CracBuilder().doCheckpointAndRestore();
    }

    @Override
    public void exec() throws Exception {
        List<ProcessBuilder> pipeline = IntStream.range(0, 3)
                .mapToObj(ignored -> new ProcessBuilder().command("cat")
                        .redirectError(ProcessBuilder.Redirect.DISCARD)).toList();
        // The pipeline creates several FDs to connect the subprocesses,
        // but all of them should be closed (in this process) when the method returns.
        List<Process> processes = ProcessBuilder.startPipeline(pipeline);
        try (
                var writer = new OutputStreamWriter(processes.get(0).getOutputStream());
                var reader = new BufferedReader(new InputStreamReader(processes.get(2).getInputStream()))
        ) {
            writer.write("Hello world\n");
            writer.flush();
            assertEquals("Hello world", reader.readLine());
            try {
                Core.checkpointRestore();
                fail("Should have failed");
            } catch (CheckpointException e) {
                // One for pipe to the first process, another for pipe from the last
                assertEquals(2, e.getSuppressed().length);
            }
        }
        // This time it should succeed
        Core.checkpointRestore();
        assertEquals(0, processes.get(0).waitFor());
        assertEquals(0, processes.get(1).waitFor());
        assertEquals(0, processes.get(2).waitFor());
    }
}
