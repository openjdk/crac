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

package jdk.test.lib.crac;

import jdk.test.lib.Container;
import jdk.test.lib.Utils;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.containers.docker.DockerfileConfig;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.util.FileUtils;

import java.io.File;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import static jdk.test.lib.Asserts.*;

/**
 * Tests using this must be tagged with:
 * <ul>
 * <li> {@code @modules java.base/jdk.internal.platform}, see
 * <a href="https://github.com/openjdk/jdk/pull/28557#issuecomment-3597274354">this discussion</a>; </li>
 * <li> {@code @requires !jdk.static} unless the test uses an image that has X11 installed,
 * the default image currently does not have it making static JDK that loads X11 eagerly
 * fail to start. </li>
 * </ul>
 */
public class CracContainerBuilder extends CracBuilderBase<CracContainerBuilder> {
    // Make it unique so that tests running in parallel do not conflict with:
    // docker: Error response from daemon: Conflict. The container name "/crac-test" is already in use by container "<hash>". You have to remove (or rename) that container to be able to reuse that name.
    public static final String CONTAINER_NAME = "crac-test" + ProcessHandle.current().pid();
    public static final String DOCKER_JAVA = "/jdk/bin/java";
    // Set this property to true to re-use an existing image.
    // By default an image will be built from scratch.
    // Reusing an image may be useful for running test cases with the same image,
    // without rebuilding it.
    public static final boolean REUSE_IMAGE_IF_EXIST = Boolean.getBoolean("jdk.test.crac.reuse.image");

    private static final List<String> CRIU_CANDIDATES = List.of(Utils.TEST_JDK + "/lib/criu", "/usr/sbin/criu", "/sbin/criu");
    private static final String CRIU_PATH;
    static {
        String path = System.getenv("CRAC_CRIU_PATH");
        if (path == null) {
            for (String candidate : CRIU_CANDIDATES) {
                if (new File(candidate).exists()) {
                    path = candidate;
                    break;
                }
            }
        }
        CRIU_PATH = path;
    }

    String dockerImageBaseName;
    String dockerImageBaseVersion;
    String dockerImageName;
    private List<String> dockerOptions; // Immutable
    private List<String> dockerCheckpointOptions; // Immutable
    private List<String> containerSetupCommand; // Immutable
    boolean containerUsePrivileged;
    boolean runContainerDirectly = false;
    // make sure to update copy constructor when adding new fields

    boolean containerStarted;

    public CracContainerBuilder() {
        super();
        dockerOptions = List.of();
        dockerCheckpointOptions = List.of();
        containerSetupCommand = List.of();
    }

    protected CracContainerBuilder(CracContainerBuilder other) {
        super(other);
        dockerImageBaseName = other.dockerImageBaseName;
        dockerImageBaseVersion = other.dockerImageBaseVersion;
        dockerImageName = other.dockerImageName;
        dockerOptions = other.dockerOptions; // No deep copy because immutable
        dockerCheckpointOptions = other.dockerCheckpointOptions; // No deep copy because immutable
        containerSetupCommand = other.containerSetupCommand; // No deep copy because immutable
        containerUsePrivileged = other.containerUsePrivileged;
        runContainerDirectly = other.runContainerDirectly;
        // containerStarted is left out intentionally
    }

    @Override
    protected CracContainerBuilder self() {
        return this;
    }

    @Override
    public CracContainerBuilder copy() {
        return new CracContainerBuilder(this);
    }

    public CracContainerBuilder withBaseImage(String name, String tag) {
        assertNull(dockerImageBaseName);
        assertNull(dockerImageBaseVersion);
        dockerImageBaseName = name;
        dockerImageBaseVersion = tag;
        return this;
    }

    public CracContainerBuilder inDockerImage(String imageName) {
        assertNull(dockerImageName);
        dockerImageName = imageName;
        return this;
    }

    public CracContainerBuilder dockerOptions(String... options) {
        dockerOptions = List.of(options);
        return this;
    }

    public CracContainerBuilder clearDockerOptions() {
        dockerOptions = List.of();
        return this;
    }

    public CracContainerBuilder dockerCheckpointOptions(String... options) {
        dockerCheckpointOptions = List.of(options);
        return this;
    }

    public CracContainerBuilder containerSetup(String... cmd) {
        containerSetupCommand = List.of(cmd);
        return this;
    }

    public CracContainerBuilder containerUsePrivileged(boolean usePrivileged) {
        containerUsePrivileged = usePrivileged;
        return this;
    }

    public CracContainerBuilder runContainerDirectly(boolean runDirectly) {
        runContainerDirectly = runDirectly;
        return this;
    }

    public void ensureContainerStarted() throws Exception {
        assertNotNull(dockerImageName, "Docker image name must be specified");
        if (engine == CracEngine.CRIU && CRIU_PATH == null) {
            fail("CRAC_CRIU_PATH is not set and cannot find criu executable in any of: " + CRIU_CANDIDATES);
        }
        if (!containerStarted) {
            prepareContainer();
            List<String> cmd = prepareContainerCommand(dockerImageName, dockerOptions);
            log("Starting docker container:\n" + String.join(" ", cmd));
            try (final var p = new ProcessBuilder().inheritIO().command(cmd).start()) {
                assertEquals(0, p.waitFor());
            }
            setupContainer();
            containerStarted = true;
        }
    }
    private void prepareContainer() throws Exception {
        DockerTestUtils.checkCanTestDocker();

        if (runContainerDirectly && !containerSetupCommand.isEmpty()) {
            fail("runContainerDirectly and containerSetupCommand cannot be used together.");
        }
        ensureContainerKilled();

        // FIXME cooperate better with DockerTestUtils
        try {
            FileUtils.deleteFileTreeWithRetry(Path.of(".", dockerImageName.replace(":", "-")));
        } catch (NoSuchFileException ignore) {
        }

        buildDockerImage();
    }

    private void buildDockerImage() throws Exception {
        String previousBaseImageName = null;
        String previousBaseImageVersion = null;
        try {
            previousBaseImageName = System.getProperty(DockerfileConfig.BASE_IMAGE_NAME);
            previousBaseImageVersion = System.getProperty(DockerfileConfig.BASE_IMAGE_VERSION);
            if (dockerImageBaseName != null) {
                System.setProperty(DockerfileConfig.BASE_IMAGE_NAME, dockerImageBaseName);
            }
            if (dockerImageBaseVersion != null) {
                System.setProperty(DockerfileConfig.BASE_IMAGE_VERSION, dockerImageBaseVersion);
            }
            if (REUSE_IMAGE_IF_EXIST) {
                if (0 == DockerTestUtils.execute(Container.ENGINE_COMMAND, "inspect", "--type=image", dockerImageName).getExitValue()) {
                    return;
                }
            }
            DockerTestUtils.buildJdkContainerImage(dockerImageName);
        } finally {
            if (previousBaseImageName != null) {
                System.setProperty(DockerfileConfig.BASE_IMAGE_NAME, previousBaseImageName);
            } else {
                System.clearProperty(DockerfileConfig.BASE_IMAGE_NAME);
            }
            if (previousBaseImageVersion != null) {
                System.setProperty(DockerfileConfig.BASE_IMAGE_VERSION, previousBaseImageVersion);
            } else {
                System.clearProperty(DockerfileConfig.BASE_IMAGE_VERSION);
            }
        }
    }

    private List<String> prepareContainerCommand(String imageName, List<String> options) {
        List<String> cmd = prepareContainerCommandBase(imageName, options);
        cmd.addAll(Arrays.asList("sleep", "3600"));
        return cmd;
    }

    private List<String> prepareContainerCommandBase(String imageName, List<String> options) {
        List<String> cmd = new ArrayList<>();
        cmd.add(Container.ENGINE_COMMAND);
        cmd.addAll(Arrays.asList("run", "--rm"));
        if (!runContainerDirectly) {
            cmd.add("-d");
            cmd.add("--init"); // otherwise the checkpointed process would not be reaped (by sleep with PID 1)
        }
        if (containerUsePrivileged) {
            cmd.add("--privileged"); // required to give CRIU sufficient permissions
        }
        int entryCounter = 0;
        for (var entry : Utils.TEST_CLASS_PATH.split(File.pathSeparator)) {
            cmd.addAll(Arrays.asList("--volume", entry + ":/cp/" + (entryCounter++)));
        }
        new File(System.getProperty("user.dir") + "/cr").mkdirs(); // create "cr" dir under the current user, to be able to delete it later.
        cmd.addAll(Arrays.asList("--volume", System.getProperty("user.dir") + "/cr:/cr"));
        if (engine == null || engine == CracEngine.CRIU) {
            cmd.addAll(Arrays.asList("--volume", CRIU_PATH + ":/criu"));
            cmd.addAll(Arrays.asList("--env", "CRAC_CRIU_PATH=/criu"));
        }
        cmd.addAll(Arrays.asList("--name", CONTAINER_NAME));
        if (debug) {
            cmd.addAll(Arrays.asList("--publish", "5005:5005"));
        }
        cmd.addAll(options);
        cmd.add(imageName);
        return cmd;
    }

    private void setupContainer() throws Exception {
        if (!containerSetupCommand.isEmpty()) {
            List<String> cmd = new ArrayList<>();
            cmd.addAll(Arrays.asList(Container.ENGINE_COMMAND, "exec", CONTAINER_NAME));
            cmd.addAll(containerSetupCommand);
            log("Container set up:\n" + String.join(" ", cmd));
            DockerTestUtils.execute(cmd).shouldHaveExitValue(0);
        }
    }

    public void ensureContainerKilled() throws Exception {
        DockerTestUtils.execute(Container.ENGINE_COMMAND, "kill", CONTAINER_NAME).getExitValue();
        if (!DockerTestUtils.RETAIN_IMAGE_AFTER_TEST) {
            DockerTestUtils.removeDockerImage(dockerImageName);
        }
    }

    public void recreateContainer(String imageName, String... options) throws Exception {
        assertTrue(containerStarted);
        DockerTestUtils.execute(Container.ENGINE_COMMAND, "kill", CONTAINER_NAME).getExitValue();

        // Docker needs some time to remove a container after kill
        OutputAnalyzer oa;
        do {
            oa = DockerTestUtils.execute(Container.ENGINE_COMMAND, "ps");
            oa.getExitValue();
        } while (oa.getStdout().contains(CONTAINER_NAME));

        List<String> cmd = prepareContainerCommand(imageName, List.of(options));
        log("Recreating docker container:\n" + String.join(" ", cmd));
        try (final var p = new ProcessBuilder().inheritIO().command(cmd).start()) {
            assertEquals(0, p.waitFor());
        }
    }

    @Override
    public CracProcess startCheckpoint(String... javaPrefix) throws Exception {
        if (runContainerDirectly) {
            prepareContainer();
        } else {
            ensureContainerStarted();
        }
        return super.startCheckpoint(javaPrefix);
    }

    @Override
    public CracProcess startRestoreWithArgs(List<String> javaPrefix, List<String> args) throws Exception {
        if (!runContainerDirectly) {
            ensureContainerStarted();
        }
        return super.startRestoreWithArgs(javaPrefix, args);
    }

    @Override
    protected List<String> getPlainCommandPrefix() {
        return Arrays.asList(Container.ENGINE_COMMAND, "exec", CONTAINER_NAME);
    }

    @Override
    protected String getTestClassPath() {
        StringBuilder builder = new StringBuilder();
        final int numEntries = Utils.TEST_CLASS_PATH.split(File.pathSeparator).length;
        for (int i = 0; i < numEntries; ++i) {
            builder.append("/cp/").append(i).append(File.pathSeparator);
        }
        return builder.toString();
    }

    @Override
    protected List<String> getDefaultJavaPrefix() {
        List<String> cmd;
        if (runContainerDirectly) {
            cmd = prepareContainerCommandBase(dockerImageName, dockerOptions);
        } else {
            cmd = new ArrayList<>();
            cmd.add(Container.ENGINE_COMMAND);
            cmd.add("exec");
            cmd.addAll(dockerCheckpointOptions);
            cmd.add(CONTAINER_NAME);
        }
        cmd.add(DOCKER_JAVA);
        return cmd;
    }

    public void checkpointViaJcmd() throws Exception {
        runJcmd(main().getName(), "JDK.checkpoint").shouldHaveExitValue(0)
                .outputTo(System.out).errorTo(System.err);
    }

    @Override
    public OutputAnalyzer runJcmd(String id, String... command) throws Exception {
        final List<String> cmd = new ArrayList<>();
        cmd.addAll(List.of(Container.ENGINE_COMMAND, "exec", CONTAINER_NAME, "/jdk/bin/jcmd"));
        cmd.add(id);
        cmd.addAll(Arrays.asList(command));
        return DockerTestUtils.execute(cmd);
    }
}
