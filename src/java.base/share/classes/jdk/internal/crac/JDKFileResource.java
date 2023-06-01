package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.impl.CheckpointOpenFileException;
import sun.security.action.GetPropertyAction;

import java.io.File;
import java.io.FileDescriptor;

public abstract class JDKFileResource extends JDKFdResource {
    private static final String[] CLASSPATH_ENTRIES =
        GetPropertyAction.privilegedGetProperty("java.class.path")
            .split(File.pathSeparator);

    protected abstract FileDescriptor getFD();
    protected abstract String getPath();

    private boolean matchClasspath(String path) {
        for (String cp : CLASSPATH_ENTRIES) {
            if (cp.equals(path)) {
                return true;
            }
        }
        return false;
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        String path = getPath();
        if (path == null) {
            // let FileDescriptor claim everything
            return;
        }

        if (matchClasspath(path)) {
            // Files on the classpath are considered persistent, exception is not thrown
            return;
        }

        FileDescriptor fd = getFD();
        Core.getClaimedFDs().claimFd(fd, this, () -> new CheckpointOpenFileException(path, getStackTraceHolder()), fd);
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {

    }
}
