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
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.nio.file.Files;
import java.nio.file.Path;

import static jdk.test.lib.Asserts.assertTrue;

/**
 * @test
 * @requires (os.family == "linux")
 * @library /test/lib
 * @build OutputToFileTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class OutputToFileTest implements CracTest {
    @Override
    public void test() throws Exception {
        Path stdin = Files.createTempFile(getClass().getSimpleName(), ".in");
        Files.writeString(stdin, "foobar");
        Path stdout = Files.createTempFile(getClass().getSimpleName(), ".out");
        Path stderr = Files.createTempFile(getClass().getSimpleName(), ".err");
        new CracBuilder().inputFrom(stdin).outputTo(stdout, stderr).doCheckpoint();
        assertTrue(stdin.toFile().delete());
        assertTrue(stdout.toFile().delete());
        assertTrue(stderr.toFile().delete());
        new CracBuilder().doRestore();
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
    }
}
