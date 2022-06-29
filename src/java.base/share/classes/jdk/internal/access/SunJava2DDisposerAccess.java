package jdk.internal.access;

public interface SunJava2DDisposerAccess {
    void beforeCheckpoint() throws Exception;
    void afterRestore() throws Exception;
}
