/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;

import java.util.List;

/**
 * @test
 * @summary If CRaCIgnoreRestoreIfUnavailable is specified and an error occurs inside the engine,
 *          VM should proceed without restoring.
 * @library /test/lib
 * @build EngineFailureTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class EngineFailureTest implements CracTest {
    private static final String MAIN_MSG = "Hello, world!";

    @Override
    public void test() throws Exception {
        final var builder = new CracBuilder()
                .engine(CracEngine.SIMULATE)
                .vmOption("-XX:+CRaCIgnoreRestoreIfUnavailable")
                .forwardClasspathOnRestore(true);
        // Create a minimal C/R image (engine file, CPU features...)
        builder.doCheckpoint();
        // Try to restore without pause=true which should make simengine fail
        try (final var p = builder.startRestoreWithArgs(List.of(), List.of(MissingMetadataTest.Main.class.getName()))) {
            p.outputAnalyzer()
                    .shouldHaveExitValue(0)
                    .shouldContain("restore requires -XX:CRaCEngineOptions=pause=true").shouldContain(MAIN_MSG);
        }
    }

    @Override
    public void exec() throws Exception {
        CRaCMXBean.getCRaCMXBean().checkpointRestore();
    }

    public static class Main {
        public static void main(String[] args) throws Exception {
            System.out.println(MAIN_MSG);
        }
    }
}
