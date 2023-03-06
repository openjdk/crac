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

import jdk.test.lib.Container;
import jdk.test.lib.Utils;
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.containers.docker.DockerRunOptions;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.process.StreamPumper;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.*;
import java.util.function.Consumer;

/*
 * @test
 * @summary Test if InetAddress cache is flushed after checkpoint/restore
 * @requires docker.support
 * @library /test/lib
 * @modules java.base/jdk.crac
 * @build ResolveInetAddress
 * @run main/timeout=360 ResolveTest
 */
public class ResolveTest {
    private static final String imageName = Common.imageName("inet-address");
    public static final String TEST_HOSTNAME = "some.test.hostname.example.com";
    public static final String CONTAINER_NAME = "test-inet-address";
    public static final String CRAC_CRIU_PATH;

    static {
        String path = System.getenv("CRAC_CRIU_PATH");
        if (path == null) {
            path = Utils.TEST_JDK + "/lib/criu";
        }
        CRAC_CRIU_PATH = path;
    }

    public static void main(String[] args) throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            return;
        }
        if (!Files.exists(Path.of(CRAC_CRIU_PATH))) {
            throw new RuntimeException("criu cannot be found in " + CRAC_CRIU_PATH);
        }
        DockerTestUtils.buildJdkDockerImage(imageName, "Dockerfile-is-ignored", "jdk-docker");
        try {
            Future<?> completed = startTestProcess();
            checkpointTestProcess();
            completed.get(5, TimeUnit.SECONDS);
            startRestoredProcess();
        } finally {
            ensureContainerDead();
            DockerTestUtils.removeDockerImage(imageName);
        }
    }

    private static Future<?> startTestProcess() throws Exception {
        ensureContainerDead();

        List<String> cmd = new ArrayList<>();
        cmd.add(Container.ENGINE_COMMAND);
        cmd.addAll(Arrays.asList("run", "--rm", "--add-host", TEST_HOSTNAME + ":192.168.12.34"));
        cmd.addAll(Arrays.asList("--volume", Utils.TEST_CLASSES + ":/test-classes/"));
        cmd.addAll(Arrays.asList("--volume", "cr:/cr"));
        cmd.addAll(Arrays.asList("--volume", CRAC_CRIU_PATH + ":/criu"));
        cmd.addAll(Arrays.asList("--env", "CRAC_CRIU_PATH=/criu"));
        cmd.addAll(Arrays.asList("--entrypoint", "bash"));
        // checkpoint-restore does not work without this: TODO fine-grained --cap-add
        cmd.add("--privileged");
        cmd.addAll(Arrays.asList("--name", CONTAINER_NAME));
        cmd.add(imageName);
        // Checkpointing does not work for PID 1, therefore we add an intermediary bash process
        List<String> javaCmd = new ArrayList<>();
        javaCmd.addAll(Arrays.asList("/jdk/bin/java", "-cp /test-classes/", "-XX:CRaCCheckpointTo=/cr"));
        javaCmd.addAll(Arrays.asList(Utils.getTestJavaOpts()));
        javaCmd.addAll(Arrays.asList("ResolveInetAddress", TEST_HOSTNAME, "/second-run"));
        cmd.addAll(Arrays.asList("-c", String.join(" ", javaCmd) + "; echo i-am-here-to-force-child-process"));

        System.err.println("Running: " + String.join(" ", cmd));

        CompletableFuture<?> firstOutputFuture = new CompletableFuture<Void>();
        Future<?> completed = executeWatching(cmd, line -> {
            System.out.println("OUTPUT: " + line);
            if (line.equals("192.168.12.34")) {
                firstOutputFuture.complete(null);
            }
        }, error -> {
            System.err.println("ERROR: " + error);
            firstOutputFuture.cancel(false);
        });
        firstOutputFuture.get(10, TimeUnit.SECONDS);
        return completed;
    }

    private static void ensureContainerDead() throws Exception {
        // ensure the container is not running, ignore if not present
        DockerTestUtils.execute("docker", "kill", CONTAINER_NAME).getExitValue();
    }

    private static void checkpointTestProcess() throws Exception {
        DockerTestUtils.execute("docker", "exec", CONTAINER_NAME,
                        "/jdk/bin/jcmd", ResolveInetAddress.class.getName(), "JDK.checkpoint")
                .shouldHaveExitValue(0);
    }

    private static Future<Void> executeWatching(List<String> command, Consumer<String> outputConsumer, Consumer<String> errorConsumer) throws IOException, ExecutionException, InterruptedException, TimeoutException {
        ProcessBuilder pb = new ProcessBuilder(command);
        Process p = pb.start();
        Future<Void> outputPumper = pump(p.getInputStream(), outputConsumer);
        Future<Void> errorPumper = pump(p.getErrorStream(), errorConsumer);
        return outputPumper;
    }

    private static Future<Void> pump(InputStream stream, Consumer<String> consumer) {
        return new StreamPumper(stream).addPump(new StreamPumper.LinePump() {
            @Override
            protected void processLine(String line) {
                consumer.accept(line);
            }
        }).process();
    }

    private static void startRestoredProcess() throws Exception {
        DockerRunOptions opts = new DockerRunOptions(imageName, "/jdk/bin/java", "ResolveInetAddress");
        opts.addDockerOpts("--volume", Utils.TEST_CLASSES + ":/test-classes/");
        opts.addDockerOpts("--volume", "cr:/cr");
        opts.addDockerOpts("--volume", Utils.TEST_CLASSES + ":/second-run"); // any file suffices
        opts.addDockerOpts("--volume", CRAC_CRIU_PATH + ":/criu");
        opts.addDockerOpts("--env", "CRAC_CRIU_PATH=/criu");
        opts.addDockerOpts("--add-host", TEST_HOSTNAME + ":192.168.56.78");
        opts.addDockerOpts("--privileged");
        opts.addJavaOpts("-XX:CRaCRestoreFrom=/cr");
        DockerTestUtils.dockerRunJava(opts)
                .shouldHaveExitValue(0)
                .shouldContain("192.168.56.78");
    }
}
