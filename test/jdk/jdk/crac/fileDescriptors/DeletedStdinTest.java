/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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
import jdk.crac.Resource;
import jdk.crac.management.CRaCMXBean;
import jdk.test.lib.Asserts;
import jdk.test.lib.Container;
import jdk.test.lib.Utils;
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.crac.CracContainerBuilder;
import jdk.test.lib.crac.CracEngine;
import jdk.test.lib.crac.CracTest;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * @test
 * @bug 8389955
 * @summary Test C/R of stdio FD pointing at a deleted file on a mount not visible in the current mount namespace.
 * @requires os.family == "linux"
 * @requires container.support
 * @comment Static JDK eagerly loads X11 which is missing from the Docker image
 * @requires !jdk.static
 * @library /test/lib
 * @modules java.base/jdk.internal.platform
 * @build DeletedStdinTest
 * @run driver jdk.test.lib.crac.CracTest
 */
public class DeletedStdinTest implements CracTest {
    private static final String TEST_SRC_VOLUME = "/testsrc";
    private static final String WRAPPER = "deleted_stdin_wrapper.sh";

    private static final Pattern MNT_ID_PATTERN = Pattern.compile("^mnt_id:\\s+(\\d+)$");

    // Assert the wrapper works as intended
    private static final Resource resource = new Resource() {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws IOException {
            final var stdinFilename = Files.readSymbolicLink(Path.of("/proc/self/fd/0")).toString();
            Asserts.assertTrue(stdinFilename.endsWith(" (deleted)"), "Wrapper failed: stdin file was not deleted");

            final int stdinMountId;
            try (final var lines = Files.lines(Path.of("/proc/self/fdinfo/0"))) {
                stdinMountId = lines.map(MNT_ID_PATTERN::matcher)
                        .filter(Matcher::matches)
                        .mapToInt(m -> Integer.parseInt(m.group(1)))
                        .findAny()
                        .orElseThrow(() -> new RuntimeException("mnt_id missing from stdin's fdinfo"));
            }
            try (final var lines = Files.lines(Path.of("/proc/self/mountinfo"))) {
                lines.forEach(line -> {
                    final var mountId = Integer.parseInt(line.split("\\s+")[0]);
                    Asserts.assertNE(stdinMountId, mountId, "Wrapper failed: stdin's mount visible in JVM: " + line);
                });
            }
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
        }
    };

    @Override
    public void test() throws Exception {
        final var builder = new CracContainerBuilder().engine(CracEngine.CRIU)
                .inDockerImage(Common.imageName("deleted-stdin"))
                .containerUsePrivileged(true)
                .dockerOptions("--volume", Utils.TEST_SRC + ":" + TEST_SRC_VOLUME);
        try {
            builder.doCheckpointToAnalyze(
                            Container.ENGINE_COMMAND, "exec", CracContainerBuilder.CONTAINER_NAME,
                            Path.of(TEST_SRC_VOLUME, WRAPPER).toString(), CracContainerBuilder.DOCKER_JAVA
                    )
                    .shouldHaveExitValue(137)
                    .shouldNotContain("Cannot reopen");
            builder.doRestore();
        } finally {
            builder.ensureContainerKilled();
        }
    }

    @Override
    public void exec() throws Exception {
        Context.getGlobalContext().register(resource);
        CRaCMXBean.getCRaCMXBean().checkpointRestore();
    }
}
