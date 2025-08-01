/*
 * Copyright (c) 2022, 2025, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.Core;
import static jdk.test.lib.Asserts.*;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;

/**
 * @test
 * @summary If a restore attempt failed and was ignored by
 *          CRaCIgnoreRestoreIfUnavailable the normal execution started instead
 *          should be checkpointable.
 * @requires (os.family == "linux")
 * @library /test/lib
 */
public class CheckpointAfterIgnoredRestoreTest {
    public static void main(String[] args) throws Exception {
        final var builder = new CracBuilder().engine(CracEngine.CRIU)
            .vmOption("-XX:+CRaCIgnoreRestoreIfUnavailable")
            .forwardClasspathOnRestore(true);
        assertTrue(Files.notExists(builder.imageDir()), "Image should not exist yet: " + builder.imageDir());
        builder
            .startRestoreWithArgs(null, List.of("-XX:CRaCCheckpointTo=" + builder.imageDir(), Main.class.getName()))
            .waitForCheckpointed();
        builder.doRestore();
    }

    public static class Main {
        public static void main(String[] args) throws Exception {
            Core.checkpointRestore();
        }
    }
}
