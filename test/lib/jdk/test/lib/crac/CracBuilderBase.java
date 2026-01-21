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

import jdk.test.lib.Utils;
import jdk.test.lib.process.OutputAnalyzer;

import java.io.File;
import java.io.IOException;
import java.nio.file.Path;
import java.util.*;
import java.util.stream.Stream;

import static jdk.test.lib.Asserts.*;

public abstract class CracBuilderBase<T extends CracBuilderBase<T>> {
    private static final String DEFAULT_IMAGE_DIR = "cr";
    public static final String JAVA = Utils.TEST_JDK + "/bin/java";

    // This dummy field is here as workaround for (possibly) a JTReg bug;
    // some tests don't build CracTestArg into their Test.d/ directory
    // (not all classes from /test/lib are built!) and the tests would fail.
    // This does not always happen when the test is run individually but breaks
    // when the whole suite is executed.
    private static final Class<CracTestArg> dummyWorkaround = CracTestArg.class;

    boolean verbose = true;
    boolean debug = false;
    final List<String> classpathEntries;
    final Map<String, String> env;
    final List<String> vmOptions;
    final Map<String, String> javaOptions;
    String imageDir = DEFAULT_IMAGE_DIR;
    CracEngine engine;
    String[] engineOptions;
    boolean printResources;
    boolean forwardClasspathOnRestore;
    Class<?> main;
    String[] args;
    boolean captureOutput;
    // make sure to update copy constructor when adding new fields

    protected abstract T self();

    public abstract T copy();

    public CracBuilderBase() {
        classpathEntries = new ArrayList<>();
        env = new HashMap<>();
        vmOptions = new ArrayList<>();
        javaOptions = new HashMap<>();
    }

    protected CracBuilderBase(T other) {
        verbose = other.verbose;
        debug = other.debug;
        classpathEntries = new ArrayList<>(other.classpathEntries);
        env = new HashMap<>(other.env);
        vmOptions = new ArrayList<>(other.vmOptions);
        javaOptions = new HashMap<>(other.javaOptions);
        imageDir = other.imageDir;
        engine = other.engine;
        engineOptions = other.engineOptions == null ? null : Arrays.copyOf(other.engineOptions, other.engineOptions.length);
        printResources = other.printResources;
        forwardClasspathOnRestore = other.forwardClasspathOnRestore;
        main = other.main;
        args = other.args == null ? null : Arrays.copyOf(other.args, other.args.length);
        captureOutput = other.captureOutput;
    }

    public T verbose(boolean verbose) {
        this.verbose = verbose;
        return self();
    }

    public T debug(boolean debug) {
        this.debug = debug;
        return self();
    }

    public T classpathEntry(String cp) {
        classpathEntries.add(cp);
        return self();
    }

    public T engine(CracEngine engine) {
        assertTrue(this.engine == null || this.engine.equals(engine));
        this.engine = engine;
        return self();
    }

    public T engineOptions(String... options) {
        this.engineOptions = options;
        return self();
    }

    public Path imageDir() {
        return Path.of(imageDir);
    }

    public T imageDir(String imageDir) {
        assertEquals(DEFAULT_IMAGE_DIR, this.imageDir); // set once
        this.imageDir = imageDir;
        return self();
    }

    public T vmOption(String option) {
        vmOptions.add(option);
        return self();
    }

    public T clearVmOptions() {
        vmOptions.clear();
        return self();
    }

    public T printResources(boolean print) {
        this.printResources = print;
        return self();
    }

    public T forwardClasspathOnRestore(boolean forward) {
        this.forwardClasspathOnRestore = forward;
        return self();
    }

    public T env(String name, String value) {
        env.put(name, value);
        return self();
    }

    public T javaOption(String name, String value) {
        javaOptions.put(name, value);
        return self();
    }

    public T main(Class<?> mainClass) {
        assertNull(this.main); // set once
        this.main = mainClass;
        return self();
    }

    public Class<?> main() {
        return main != null ? main : CracTest.class;
    }

    public T args(String... args) {
        assertNull(this.args); // set once
        this.args = args;
        return self();
    }

    public String[] args() {
        return args != null ? args : CracTest.args();
    }

    public T captureOutput(boolean captureOutput) {
        this.captureOutput = captureOutput;
        return self();
    }

    public void doCheckpoint(String... javaPrefix) throws Exception {
        startCheckpoint(javaPrefix).waitForCheckpointed();
    }

    public CracProcess startCheckpoint(String... javaPrefix) throws Exception {
        List<String> list = javaPrefix.length == 0 ? null : Arrays.asList(javaPrefix);
        return startCheckpoint(list);
    }

    public CracProcess startCheckpoint(List<String> javaPrefix) throws Exception {
        List<String> cmd = prepareCommand(javaPrefix, false);
        if (imageDir != null) {
            cmd.add("-XX:CRaCCheckpointTo=" + imageDir);
        }
        cmd.add(main().getName());
        cmd.addAll(Arrays.asList(args()));
        log("Starting process to be checkpointed:");
        log(String.join(" ", cmd));
        return new CracProcess(this, cmd);
    }

    void log(String fmt, Object... args) {
        if (verbose) {
            if (args.length == 0) {
                System.err.println(fmt);
            } else {
                System.err.printf(fmt, args);
            }
        }
    }

    public CracProcess doRestore(String... javaPrefix) throws Exception {
        return startRestore(javaPrefix).waitForSuccess();
    }

    public CracProcess startRestore(String... javaPrefix) throws Exception {
        List<String> list = javaPrefix.length == 0 ? null : Arrays.asList(javaPrefix);
        return startRestore(list);
    }

    public CracProcess startRestore(List<String> javaPrefix) throws Exception {
        return startRestoreWithArgs(javaPrefix, null);
    }

    public CracProcess startRestoreWithArgs(List<String> javaPrefix, List<String> args) throws Exception {
        List<String> cmd = prepareCommand(javaPrefix, true);
        if (imageDir != null) {
            cmd.add("-XX:CRaCRestoreFrom=" + imageDir);
        }
        if (null != args) {
            cmd.addAll(args);
        }
        log("Starting restored process:");
        log(String.join(" ", cmd));
        return new CracProcess(this, cmd);
    }

    public CracProcess startPlain() throws IOException {
        List<String> cmd = new ArrayList<>(getPlainCommandPrefix());
        cmd.add(JAVA);
        cmd.add("-ea");
        cmd.add("-cp");
        cmd.add(getClassPath());
        if (debug) {
            cmd.add("-agentlib:jdwp=transport=dt_socket,server=y,suspend=y,address=0.0.0.0:5005");
        }
        cmd.addAll(vmOptions);
        for (var entry : javaOptions.entrySet()) {
            cmd.add("-D" + entry.getKey() + "=" + entry.getValue());
        }
        cmd.add(main().getName());
        cmd.addAll(Arrays.asList(args()));
        log("Starting process without CRaC:");
        log(String.join(" ", cmd));
        return new CracProcess(this, cmd);
    }

    protected List<String> getPlainCommandPrefix() {
        return List.of();
    }

    private String getClassPath() {
        String classPath = classpathEntries.isEmpty() ? "" : String.join(File.pathSeparator, classpathEntries) + File.pathSeparator;
        return classPath + getTestClassPath();
    }

    protected String getTestClassPath() {
        return Utils.TEST_CLASS_PATH;
    }

    public CracProcess doPlain() throws IOException, InterruptedException {
        return startPlain().waitForSuccess();
    }

    private List<String> prepareCommand(List<String> javaPrefix, boolean isRestore) {
        List<String> cmd = new ArrayList<>(javaPrefix != null ? javaPrefix : getDefaultJavaPrefix());
        cmd.add("-ea");
        if (engine != null) {
            cmd.add("-XX:CRaCEngine=" + engine.engine);
        }
        if (engineOptions != null) {
            cmd.add("-XX:CRaCEngineOptions=" + String.join(",", engineOptions));
        }
        if (!isRestore || forwardClasspathOnRestore) {
            cmd.add("-cp");
            cmd.add(getClassPath());
        }
        if (!isRestore && printResources) {
            cmd.add("-XX:+UnlockDiagnosticVMOptions");
            cmd.add("-XX:+CRaCPrintResourcesOnCheckpoint");
        }
        if (debug) {
            cmd.add("-agentlib:jdwp=transport=dt_socket,server=y,suspend=y,address=0.0.0.0:5005");
        }
        cmd.addAll(vmOptions);
        for (var entry : javaOptions.entrySet()) {
            cmd.add("-D" + entry.getKey() + "=" + entry.getValue());
        }
        return cmd;
    }

    protected List<String> getDefaultJavaPrefix() {
        return List.of(JAVA);
    }

    public void doCheckpointAndRestore() throws Exception {
        doCheckpoint();
        doRestore();
    }

    public void checkpointViaJcmd(long pid, String... args) throws Exception {
        runJcmd(Long.toString(pid), Stream.concat(Stream.of("JDK.checkpoint"), Stream.of(args)).toArray(String[]::new))
                .shouldHaveExitValue(0).outputTo(System.out).errorTo(System.err);
    }

    public OutputAnalyzer runJcmd(String id, String... command) throws Exception {
        final List<String> cmd = new ArrayList<>();
        cmd.add(Utils.TEST_JDK + "/bin/jcmd");
        cmd.add(id);
        cmd.addAll(Arrays.asList(command));
        log("Executing JCMD command for PID " + id + ": " + String.join(" ", List.of(command)));
        return new OutputAnalyzer(new ProcessBuilder(cmd).start());
    }
}
