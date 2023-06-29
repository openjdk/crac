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
import java.io.FileReader;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertGreaterThan;

public abstract class FDPolicyTestBase {

    protected void writeBigFile(Path path, String prefix, String suffix) throws IOException {
        StringBuilder sb = new StringBuilder().append(prefix);
        // Let's use 8+ MB file to avoid hidden buffering in FileInputStream or native parts
        for (int i = 0; i < 1024 * 1024; ++i) {
            sb.append(String.format("%08X", 8 * i));
        }
        sb.append(suffix);
        Files.writeString(path, sb.toString());
    }

    protected void readContents(FileReader reader) throws IOException {
        char[] bigbuf = new char[1024 * 1024];
        for (int count = 0; count < 8 * 1024 * 1024; ) {
            int r = reader.read(bigbuf);
            assertGreaterThan(r, 8);
            assertEquals(String.format("%08X", count), new String(bigbuf, 0, 8));
            count += r;
        }
    }

    protected Path writeConfig(String content) throws IOException {
        Path config = Files.createTempFile(getClass().getName(), ".yaml");
        Files.writeString(config, content);
        return config;
    }
}
