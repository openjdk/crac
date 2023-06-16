package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.impl.CheckpointOpenFileException;
import jdk.crac.impl.OpenFilePolicies;
import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;
import sun.security.action.GetPropertyAction;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.util.function.Supplier;

public abstract class JDKFileResource extends JDKFdResource {
    private static final JavaIOFileDescriptorAccess fdAccess =
            SharedSecrets.getJavaIOFileDescriptorAccess();

    private static final String[] CLASSPATH_ENTRIES =
        GetPropertyAction.privilegedGetProperty("java.class.path")
            .split(File.pathSeparator);

    private final Object owner;
    private int originalFd = -1;
    private String originalPath;
    private String originalType;
    private int originalFlags;
    private long originalOffset;

    protected JDKFileResource(Object owner) {
        this.owner = owner;
    }

    protected abstract FileDescriptor getFD();

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
        FileDescriptor fileDescriptor = getFD();
        synchronized (fileDescriptor) {
            if (fileDescriptor.valid()) {
                applyCheckpointPolicy(fileDescriptor);
            }
        }
    }

    @SuppressWarnings("fallthrough")
    private void applyCheckpointPolicy(FileDescriptor fileDescriptor) throws CheckpointOpenFileException {
        int fd = fdAccess.get(fileDescriptor);
        String path = getPath(fileDescriptor);
        String type = getType(fileDescriptor);
        OpenFilePolicies.BeforeCheckpoint policy = OpenFilePolicies.CHECKPOINT.get(fd, path, type);
        switch (policy) {
            case ERROR:
                Supplier<Exception> exceptionSupplier = claimException(fileDescriptor, path);
                Core.getClaimedFDs().claimFd(fileDescriptor, owner, exceptionSupplier, fileDescriptor);
                break;
            case WARN_CLOSE:
                LoggerContainer.warn("CRaC: File {0} (FD {1}) was not closed by the application!", path, fd);
                // intentional fallthrough
            case CLOSE:
                originalFd = fd;
                originalPath = path;
                originalType = type;
                originalFlags = getFlags(fileDescriptor);
                originalOffset = getOffset(fileDescriptor);
                if (originalOffset < 0) {
                    throw new CheckpointOpenFileException("Cannot find current offset of descriptor " + fd + "(" + path + ")", null);
                }
                try {
                    // do not unregister any handlers
                    fdAccess.closeNoCleanup(fileDescriptor);
                } catch (IOException e) {
                    throw new CheckpointOpenFileException("Cannot close file descriptor " + fd + " (" + path + ") before checkpoint", e);
                }
                LoggerContainer.debug("Closed FD {0} ({1}, offset {2} with flags 0x{3}%n",
                        originalFd, originalPath, originalOffset,
                        Integer.toHexString(originalFlags).toUpperCase());
                break;
            default:
                throw new IllegalArgumentException("Unknown policy " + policy);
        }
    }

    protected Supplier<Exception> claimException(FileDescriptor fd, String path) {
        int fdVal = fdAccess.get(fd);
        Supplier<Exception> exceptionSupplier;
        if (matchClasspath(path)) {
            // Files on the classpath are considered persistent, exception is not thrown
            exceptionSupplier = null;
        } else {
            exceptionSupplier = () -> new CheckpointOpenFileException(path + " (FD " + fdVal + ")", getStackTraceHolder());
        }
        return exceptionSupplier;
    }

    @Override
    public void afterRestore(Context<? extends jdk.crac.Resource> context) throws Exception {
        FileDescriptor fileDescriptor = getFD();
        synchronized (fileDescriptor) {
            if (!fileDescriptor.valid() && originalFd >= 0) {
                applyRestorePolicy(fileDescriptor);
            }
            // let GC collect the path and type
            originalPath = null;
        }
    }

    private void applyRestorePolicy(FileDescriptor fileDescriptor) throws OpenFilePolicies.RestoreFileDescriptorException {
        OpenFilePolicies.AfterRestorePolicy policy =
                OpenFilePolicies.RESTORE.get(originalFd, originalPath, originalType);
        if (policy.type == OpenFilePolicies.AfterRestore.KEEP_CLOSED) {
            LoggerContainer.debug("FD %d (%s) is not reopened per policy%n",
                    originalFd, originalPath);
            originalPath = null;
            return;
        }
        String path;
        if (policy.type == OpenFilePolicies.AfterRestore.OPEN_OTHER) {
            path = policy.param;
        } else {
            if (originalPath == null) {
                throw new OpenFilePolicies.RestoreFileDescriptorException("Cannot reopen file descriptor " +
                        originalFd + ": no path");
            }
            path = originalPath;
        }
        // We will attempt to open at the original offset even if the path changed;
        // this is used probably as the file moved on the filesystem but the contents
        // are the same.
        long offset = originalOffset;
        if (!reopen(originalFd, path, originalFlags, offset)) {
            if (policy.type == OpenFilePolicies.AfterRestore.REOPEN_OR_NULL) {
                if (!reopenNull(originalFd)) {
                    throw new OpenFilePolicies.RestoreFileDescriptorException("Cannot reopen file descriptor " +
                            originalFd + " to null device");
                }
            } else {
                throw new OpenFilePolicies.RestoreFileDescriptorException("Cannot reopen file descriptor " +
                        originalFd + " to " + path);
            }
        } else {
            LoggerContainer.debug("Reopened FD %d (%s, offset %d) with flags 0x%08X%n",
                    originalFd, originalPath, originalOffset, originalFlags);
        }
        fdAccess.set(fileDescriptor, originalFd);
    }

    @Override
    public String toString() {
        return owner + ".Resource(FD=" + fdAccess.get(getFD()) + ")";
    }

    private static native String getPath(FileDescriptor fd);

    private static native String getType(FileDescriptor fd);

    private static native int getFlags(FileDescriptor fd);

    private static native long getOffset(FileDescriptor fd);

    private static native boolean reopen(int fd, String path, int flags, long offset);

    private static native boolean reopenNull(int fd);
}
