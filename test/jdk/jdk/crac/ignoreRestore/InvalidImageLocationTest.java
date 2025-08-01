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
import jdk.test.lib.crac.CracEngine;

/**
 * @test
 * @summary If CRaCIgnoreRestoreIfUnavailable is specified and CRaCRestoreFrom
 *          points to an invalid location VM should proceed without restoring.
 * @requires (os.family == "linux")
 * @library /test/lib
 * @run main InvalidImageLocationTest IMAGE_NOT_EXISTS
 * @run main InvalidImageLocationTest IMAGE_IS_NOT_DIR
 */
public class InvalidImageLocationTest {
    private static final String MAIN_MSG = "Hello, world!";

    private enum Variant {
        IMAGE_NOT_EXISTS,
        IMAGE_IS_NOT_DIR,
    }

    public static void main(String[] args) throws Exception {
        final var variant = Variant.valueOf(args[0]);

        final var builder = new CracBuilder().engine(CracEngine.CRIU)
            .vmOption("-XX:+CRaCIgnoreRestoreIfUnavailable")
            .forwardClasspathOnRestore(true)
            .captureOutput(true);

        // Existance depends on the order of @run tags
        if (variant == Variant.IMAGE_NOT_EXISTS) {
            Files.deleteIfExists(builder.imageDir());
        } else if (variant == Variant.IMAGE_IS_NOT_DIR && Files.notExists(builder.imageDir())) {
            Files.createFile(builder.imageDir());
        }

        final var errMsg = switch (variant) {
            case IMAGE_NOT_EXISTS -> "Cannot open CRaCRestoreFrom=" + builder.imageDir() + ":";
            case IMAGE_IS_NOT_DIR -> "CRaCRestoreFrom=" + builder.imageDir() + " is not a directory";
        };

        final var out = builder.startRestoreWithArgs(null, List.of(Main.class.getName())).outputAnalyzer();
        out.shouldContain(errMsg).shouldContain(MAIN_MSG);
    }

    public static class Main {
        public static void main(String[] args) {
            System.out.println(MAIN_MSG);
        }
    }
}
