package jdk.test.lib.crac;

import jdk.test.lib.Utils;

import java.io.File;
import java.io.IOException;
import java.nio.file.Path;
import java.util.*;

import static jdk.test.lib.Asserts.assertEquals;
import static jdk.test.lib.Asserts.assertNull;

public class CracBuilder {
    private static final String DEFAULT_IMAGE_DIR = "cr";

    boolean verbose = true;
    final List<String> classpath = new ArrayList<>();
    final Map<String, String> env = new HashMap<>();
    String imageDir = DEFAULT_IMAGE_DIR;
    CracEngine engine;
    boolean printResources;
    Class<?> main;
    String[] args;
    boolean captureOutput;

    public CracBuilder() {
        classpath.addAll(Arrays.asList(Utils.TEST_CLASS_PATH.split(File.pathSeparator)));
    }
    public CracBuilder verbose(boolean verbose) {
        this.verbose = verbose;
        return this;
    }

    public CracBuilder classpathEntry(String cp) {
        classpath.add(cp);
        return this;
    }

    public CracBuilder engine(CracEngine engine) {
        assertNull(this.engine); // set once
        this.engine = engine;
        return this;
    }

    public Path imageDir() {
        return Path.of(imageDir);
    }

    public CracBuilder imageDir(String imageDir) {
        assertEquals(DEFAULT_IMAGE_DIR, this.imageDir); // set once
        this.imageDir = imageDir;
        return this;
    }

    public CracBuilder printResources(boolean print) {
        this.printResources = print;
        return this;
    }

    public CracBuilder env(String name, String value) {
        env.put(name, value);
        return this;
    }

    public CracBuilder main(Class<?> mainClass) {
        assertNull(this.main); // set once
        this.main = mainClass;
        return this;
    }

    public CracBuilder args(String... args) {
        assertNull(this.args); // set once
        this.args = args;
        return this;
    }

    public CracBuilder captureOutput(boolean captureOutput) {
        this.captureOutput = captureOutput;
        return this;
    }

    public void doCheckpoint() throws IOException, InterruptedException {
        startCheckpoint().waitForCheckpointed();
    }

    public CracProcess startCheckpoint() throws IOException {
        List<String> cmd = prepareCommand();
        cmd.add("-XX:CRaCCheckpointTo=" + imageDir);
        cmd.add(main.getName());
        if (args != null) {
            cmd.addAll(Arrays.asList(args));
        }
        if (verbose) {
            System.err.println("Starting process to be checkpointed:");
            System.err.println(String.join(" ", cmd));
        }
        return new CracProcess(this, cmd);
    }

    public CracProcess doRestore() throws IOException, InterruptedException {
        return startRestore().waitForSuccess();
    }

    public CracProcess startRestore() throws IOException {
        List<String> cmd = prepareCommand();
        cmd.add("-XX:CRaCRestoreFrom=" + imageDir);
        if (verbose) {
            System.err.println("Starting restored process:");
            System.err.println(String.join(" ", cmd));
        }
        return new CracProcess(this, cmd);
    }

    public CracProcess startPlain() throws IOException {
        List<String> cmd = new ArrayList<>();
        cmd.add(Utils.TEST_JDK + "/bin/java");
        cmd.add("-ea");
        cmd.add("-cp");
        cmd.add(String.join(File.pathSeparator, classpath));
        cmd.add(main.getName());
        if (args != null) {
            cmd.addAll(Arrays.asList(args));
        }
        if (verbose) {
            System.err.println("Starting process without CRaC:");
            System.err.println(String.join(" ", cmd));
        }
        return new CracProcess(this, cmd);
    }

    public CracProcess doPlain() throws IOException, InterruptedException {
        return startPlain().waitForSuccess();
    }

    private List<String> prepareCommand() {
        List<String> cmd = new ArrayList<>();
        cmd.add(Utils.TEST_JDK + "/bin/java");
        cmd.add("-ea");
        cmd.add("-cp");
        cmd.add(String.join(File.pathSeparator, classpath));
        if (engine != null) {
            cmd.add("-XX:CREngine=" + engine.engine);
        }
        if (printResources) {
            cmd.add("-XX:+UnlockDiagnosticVMOptions");
            cmd.add("-XX:+CRPrintResourcesOnCheckpoint");
        }
        return cmd;
    }

    public void doCheckpointAndRestore() throws IOException, InterruptedException {
        doCheckpoint();
        doRestore();
    }
}
