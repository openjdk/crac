/*
 * Copyright (c) 2022, Azul Systems, Inc. All rights reserved.
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
import jdk.test.lib.Asserts;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.io.File;
import java.io.IOException;
import java.nio.channels.Pipe;
import java.nio.file.*;
import java.util.Objects;
import java.util.concurrent.TimeUnit;

/**
 * @test CracOptionTest
 * @library /test/lib
 * @build CracOptionTest
 * @run driver jdk.test.lib.crac.CracTest
 */

public class CracOptionTest implements CracTest {
    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder();
        builder.javaOption("k","v");
        builder.doCheckpointAndRestore();
    }

    @Override
    public void exec() throws Exception {
        Core.checkpointRestore();
    }
}
