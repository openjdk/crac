package crac.fileDescriptors;/*
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

import jdk.crac.Core;
import jdk.test.lib.Asserts;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.nio.file.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * @test
 * @library /test/lib
 * @build FileWatcherAfterRestoreTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class FileWatcherAfterRestoreTest implements CracTest {
    @Override
    public void test() throws Exception {
        CracBuilder builder = new CracBuilder();
        builder.doCheckpointAndRestore();
    }

    @Override
    public void exec() throws Exception {
        WatchService watchService = FileSystems.getDefault().newWatchService();

        Core.checkpointRestore();

        Path directory = Paths.get(System.getProperty("user.dir"));
        WatchKey key = directory.register(watchService, StandardWatchEventKinds.ENTRY_CREATE);
        Thread.sleep(1000);
        Path tempFilePath = Files.createTempFile(directory, "temp", ".txt");

        // other file events might happen, so iterate all to make test stable
        boolean matchFound = false;
        while ((key = watchService.poll(1, TimeUnit.SECONDS)) != null) {
            System.out.println(key);
            for (WatchEvent<?> event : key.pollEvents()) {
                System.out.println(event.kind().toString());
                if (event.kind() == StandardWatchEventKinds.ENTRY_CREATE) {
                    Object context = event.context();
                    if (context instanceof Path filePath) {
                        String fileName = filePath.getFileName().toString();
                        System.out.println(fileName);
                        if (fileName.matches("^temp\\d*\\.txt$")) {
                            matchFound = true;
                            break;
                        }
                    }
                }
            }
            key.reset();
            if (matchFound) {
                break;
            }
        }

        Asserts.assertTrue(matchFound);
    }
}
