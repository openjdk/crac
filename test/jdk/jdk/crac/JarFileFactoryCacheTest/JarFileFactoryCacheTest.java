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

import java.io.InputStream;
import java.net.URL;
import java.nio.file.Path;

/**
 * @test JarFileFactoryCacheTest
 * @library /test/lib
 * @run main/othervm -XX:CREngine=simengine -XX:CRaCCheckpointTo=./cr -XX:+UnlockDiagnosticVMOptions -XX:+CRPrintResourcesOnCheckpoint JarFileFactoryCacheTest
 */
public class JarFileFactoryCacheTest {
    static public void main(String[] args) throws Exception {
        jdk.test.lib.util.JarUtils.createJarFile(
            Path.of("test.jar"),
            Path.of(System.getProperty("test.src")),
            "test.txt");

        URL url = new URL("jar:file:test.jar!/test.txt");
        InputStream inputStream = url.openStream();
        byte[] content = inputStream.readAllBytes();
        if (content.length != 5) {
            throw new AssertionError("wrong content");
        }
        inputStream.close();
        inputStream = null;
        url = null;

        Core.checkpointRestore();
    }
}
