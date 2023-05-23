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

import jdk.crac.Core;
import jdk.test.lib.Asserts;
import jdk.test.lib.crac.CracBuilder;
import jdk.test.lib.crac.CracTest;

import java.io.File;
import java.io.IOException;
import java.nio.file.*;
import java.util.Objects;
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
        builder.doCheckpoint();
        Path checkPath = Paths.get(System.getProperty("user.dir"), "workdir");
        for (File f : Objects.requireNonNull(checkPath.toFile().listFiles())) {
            f.delete();
        }
        Files.delete(checkPath);
        builder.doRestore();
    }

    @Override
    public void exec() throws Exception {
        WatchService watchService = FileSystems.getDefault().newWatchService();
        Path directory;
        WatchKey key;

        directory = Paths.get(System.getProperty("user.dir"), "workdir");
        directory.toFile().mkdir();
        Files.createTempFile(directory, "temp", ".txt");
        Asserts.assertTrue(isMatchFound(watchService, directory));

        Core.checkpointRestore();

        directory = Paths.get(System.getProperty("user.dir"), "workdir");
        // fix monitor dir after restore
        if (!directory.toFile().exists()) {
            directory = Paths.get("/tmp");
        }

        Asserts.assertTrue(isMatchFound(watchService, directory));
        watchService.close();
    }

    private boolean isMatchFound(WatchService watchService, Path directory) throws IOException, InterruptedException {
        WatchKey key;
        boolean matchFound;
        WatchKey watchKey = directory.register(watchService, StandardWatchEventKinds.ENTRY_CREATE);
        Files.createTempFile(directory, "temp", ".txt");
        matchFound = false;
        while ((key = watchService.poll(1, TimeUnit.SECONDS)) != null) {
            for (WatchEvent<?> event : key.pollEvents()) {
                if (event.kind() == StandardWatchEventKinds.ENTRY_CREATE) {
                    Object context = event.context();
                    if (context instanceof Path filePath) {
                        String fileName = filePath.getFileName().toString();
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
        watchKey.cancel();
        return matchFound;
    }
}
