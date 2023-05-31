package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.impl.CheckpointOpenFileException;
import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;

import java.io.FileDescriptor;

public abstract class JDKFileResource extends JDKFdResource {
    private static final JavaIOFileDescriptorAccess fdAccess = SharedSecrets.getJavaIOFileDescriptorAccess();

    protected abstract FileDescriptor getFD();
    protected abstract String getPath();

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        String path = getPath();
        if (path == null) {
            // let FileDescriptor claim everything
            return;
        }

        FileDescriptor fd = getFD();
        Core.getClaimedFDs().claimFd(fd, this, fd, () -> {
            if (Core.getClaimedFDs().matchClasspath(path)) {
                // Files on the classpath are considered persistent, exception is not thrown
                return null;
            }
            return new CheckpointOpenFileException(
                path,
                getStackTraceHolder());
        });
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {

    }
}
