/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.Context;
import jdk.crac.Core;
import jdk.crac.Resource;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracProcess;
import jdk.test.lib.crac.CracTest;
import jdk.test.lib.crac.CracTestArg;
import jdk.test.lib.util.FileUtils;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

import static jdk.test.lib.Asserts.assertTrue;

/**
 * @test
 * @library /test/lib
 * @build JcmdArgsTest
 * @run driver/timeout=10 jdk.test.lib.crac.CracTest false
 * @run driver/timeout=10 jdk.test.lib.crac.CracTest true
 */
public class JcmdArgsTest implements CracTest {
    private static final String READY = "TEST:READY";
    private static final String CHECKPOINTED = "TEST:CHECKPOINTED";

    @CracTestArg
    boolean useFile;

    @Override
    public void test() throws Exception {
        // Ensure the image does not exist
        Path imageDir = new CracBuilder().engine(CracEngine.SIMULATE).imageDir();
        if (imageDir.toFile().isDirectory()) {
            FileUtils.deleteFileTreeWithRetry(imageDir);
        }

        CracProcess process = new CracBuilder().engine(CracEngine.SIMULATE).startCheckpoint();
        process.waitForStdout(READY, false);
        String[] args;
        if (useFile) {
            Path metricsPath = createTemp("metrics", "dummy\t=45\n   foo.bar = 123.0 \n");
            Path labelsPath = createTemp("labels", " xxx= yyy\naaa=bbb");
            args = new String[] { "metrics=@" + metricsPath, "labels=@" + labelsPath };
        } else {
            args = new String[] { "metrics=foo.bar=123", "labels=xxx=yyy" };
        }
        new CracBuilder().engine(CracEngine.SIMULATE).checkpointViaJcmd(process.pid(), args);
        process.waitForStdout(CHECKPOINTED, false);
        process.sendNewline();
        process.waitForSuccess();

        assertTrue(Files.readAllLines(imageDir.resolve("score")).stream()
                .anyMatch("foo.bar=123.000000"::equals));
        assertTrue(Files.readAllLines(imageDir.resolve("tags")).stream()
                .anyMatch("label:xxx=yyy"::equals));
    }

    private static Path createTemp(String name, String text) throws IOException {
        Path path = Files.createTempFile(name, ".txt");
        path.toFile().deleteOnExit();
        Files.writeString(path, text);
        return path;
    }

    @Override
    public void exec() throws Exception {
        Core.getGlobalContext().register(new Resource() {
            @Override
            public void beforeCheckpoint(Context<? extends Resource> context) {
            }

            @Override
            public void afterRestore(Context<? extends Resource> context) throws Exception {
                System.out.println(CHECKPOINTED);
            }
        });
        System.out.println(READY);
        System.in.read();
    }
}
