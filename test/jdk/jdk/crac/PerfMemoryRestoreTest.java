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

import jdk.crac.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.TimeUnit;

import static jdk.test.lib.Asserts.*;

/**
 * @test ContextOrderTest
 * @requires os.family == "linux"
 * @library /test/lib
 * @build PerfMemoryRestoreTest
 * @run driver/timeout=30 jdk.test.lib.crac.CracTest
 */

public class PerfMemoryRestoreTest implements CracTest {
    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder();
        CracProcess checkpoint = builder.startCheckpoint();
        String pid = String.valueOf(checkpoint.pid());
        // This test is run only on Linux where the path is hardcoded
        // in os::get_temp_directory() to /tmp rather than using System.getProperty("java.io.tmpdir")
        Path perfdata = Path.of("/tmp", "hsperfdata_" + System.getProperty("user.name"), pid);
        long start = System.nanoTime();
        while (!perfdata.toFile().exists()) {
            if (System.nanoTime() - start > TimeUnit.SECONDS.toNanos(10)) {
                throw new IllegalStateException("Perf data file did not appear within time limit in the checkpointed process: " + perfdata);
            }
            //noinspection BusyWait
            Thread.sleep(10);
        }
        checkMapped(pid, perfdata.toString());

        checkpoint.input().write('\n');
        checkpoint.input().flush();
        checkpoint.waitForCheckpointed();
        assertFalse(perfdata.toFile().exists());

        CracProcess restored = builder.startRestore();
        start = System.nanoTime();
        while (!perfdata.toFile().exists()) {
            if (System.nanoTime() - start > TimeUnit.SECONDS.toNanos(10)) {
                throw new IllegalStateException("Perf data file did not appear within time limit in the restored process: " + perfdata);
            }
            //noinspection BusyWait
            Thread.sleep(10);
        }
        // Note: we need to check the checkpoint.pid(), which should be restored (when using CRIU),
        // as restored.pid() would be the criuengine restorewait process
        String pidString = String.valueOf(checkpoint.pid());
        checkMapped(pidString, perfdata.toString());
        builder.runJcmd(pidString, "PerfCounter.print")
                .shouldHaveExitValue(0)
                .shouldContain("sun.perfdata.size=");
        restored.input().write('\n');
        restored.input().flush();
        restored.waitForSuccess();
    }

    private static void checkMapped(String pid, String perfdata) throws IOException {
        String perfdataLine = Files.readAllLines(Path.of("/proc", pid, "maps")).stream()
                .filter(line -> line.contains(perfdata))
                .findFirst().orElseThrow(() -> new AssertionError("Missing " + perfdata + " in process maps"));
        assertTrue(perfdataLine.contains("rw-s 00000000"), perfdataLine);
    }

    @Override
    public void exec() throws Exception {
        assertEquals(System.in.read(), (int) '\n');
        Core.checkpointRestore();
        assertEquals(System.in.read(), (int) '\n');
    }
}
