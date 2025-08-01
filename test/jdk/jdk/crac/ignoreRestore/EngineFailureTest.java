/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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

import java.nio.file.Files;
import java.util.List;

import jdk.test.lib.crac.CracBuilder;

/**
 * @test
 * @summary If CRaCIgnoreRestoreIfUnavailable is specified and the engine
 *          fails to restore for any reason VM should proceed without restoring.
 * @library /test/lib
 */
public class EngineFailureTest {
    private static final String MAIN_MSG = "Hello, world!";

    public static void main(String[] args) throws Exception {
        // With crexec we need to make it itself fail and not the real engine
        // it executes: the executable replaces the JVM and thus exits the whole
        // process on failure instead of returning to JVM. This is achieved by
        // specifying "crexec" as the engine manually (normally one is not
        // supposed to do that, JVM does it automatically when needed) and do
        // not provide the mandatory 'exec_location' option.

        final var builder = new CracBuilder()
            .vmOption("-XX:CRaCEngine=crexec")
            .vmOption("-XX:+CRaCIgnoreRestoreIfUnavailable")
            .forwardClasspathOnRestore(true)
            .captureOutput(true);

        // Make VM's internal image checks pass
        Files.createDirectory(builder.imageDir());

        builder.startRestoreWithArgs(null, List.of(Main.class.getName())).outputAnalyzer()
            .shouldContain("CRaC engine failed to restore")
            .shouldContain(MAIN_MSG);
    }

    public static class Main {
        public static void main(String[] args) {
            System.out.println(MAIN_MSG);
        }
    }
}
