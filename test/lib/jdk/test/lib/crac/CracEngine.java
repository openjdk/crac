package jdk.test.lib.crac;

public enum CracEngine {
    CRIU("criu"),
    PAUSE("pauseengine"),
    SIMPLE("simengine");

    public final String engine;

    CracEngine(String engine) {
        this.engine = engine;
    }
}
