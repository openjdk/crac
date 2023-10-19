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

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

import jdk.crac.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;

import static jdk.test.lib.Asserts.*;

/**
 * @test KillRestoredTest
 * @requires os.family == "linux"
 * @library /test/lib
 * @build KillRestoredTest
 * @run driver/timeout=20 jdk.test.lib.crac.CracTest
 */
public class KillRestoredTest implements CracTest {
    @Override
    public void test() throws Exception {
        CracProcess checkpoint = new CracBuilder().startCheckpoint();
        checkpoint.waitForCheckpointed();
        CracProcess restore = new CracBuilder().startRestore();
        assertTrue(processExists(restore.pid())); // criu or criuengine
        while (!processExists(checkpoint.pid())) { // actually restored process
            Thread.sleep(50);
        }
        String children = Files.readString(Path.of("/proc/" + restore.pid() + "/task/" + restore.pid() + "/children")).trim();
        assertEquals(String.valueOf(checkpoint.pid()), children);
        new ProcessBuilder().inheritIO().command("kill", "-9", String.valueOf(restore.pid())).start().waitFor();
        assertEquals(137, restore.waitFor());
        assertFalse(processExists(restore.pid()));
        // While the PID is the same, checkpoint.waitFor() would wait for a different process
        while (processExists(checkpoint.pid())) {
            Thread.sleep(50);// signal delivery and termination might take a bit
        }
    }

    private boolean processExists(long pid) {
        File procDir = new File("/proc/" + pid);
        return procDir.exists() && procDir.isDirectory();
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
        Thread.sleep(60_000);
    }
}
