package jdk.test.lib.crac;

public enum CracEngine {
    CRIU("criuengine"),
    PAUSE(System.getProperty("os.name").contains("Windows") ? "pauseengine.exe" : "pauseengine"),
    SIMULATE(System.getProperty("os.name").contains("Windows") ? "simengine.exe" : "simengine");

    public final String engine;

    CracEngine(String engine) {
        this.engine = engine;
    }
}
