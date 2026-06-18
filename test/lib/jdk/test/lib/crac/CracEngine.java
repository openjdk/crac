package jdk.test.lib.crac;

public enum CracEngine {
    CRIU("criuengine", ""),
    PAUSE("simengine", "pause=true"),
    SIMULATE("simengine", "");

    public final String name;
    public final String options;

    CracEngine(String name, String options) {
        this.name = name;
        this.options = options;
    }
}
