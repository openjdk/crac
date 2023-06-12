package java.io;

import jdk.crac.Context;
import jdk.crac.impl.CheckpointOpenFileException;
import jdk.crac.impl.OpenFDPolicies;
import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKContext;
import jdk.internal.crac.LoggerContainer;

import java.net.Socket;

class FileDescriptorResource implements jdk.internal.crac.JDKResource {
    private static final JavaIOFileDescriptorAccess fdAccess =
            SharedSecrets.getJavaIOFileDescriptorAccess();

    private final FileDescriptor fileDescriptor;
    private int originalFd = -1;
    private String originalType;
    private String originalPath;
    private int originalFlags;
    private long originalOffset;
    private boolean closedByNIO;
    final Exception stackTraceHolder;

    FileDescriptorResource(FileDescriptor fileDescriptor) {
        this.fileDescriptor = fileDescriptor;
        if (JDKContext.Properties.COLLECT_FD_STACKTRACES) {
            // About the timestamp: we cannot format it nicely since this
            // exception is sometimes created too early in the VM lifecycle
            // (but it's hard to detect when it would be safe to do).
            stackTraceHolder = new Exception("This file descriptor was created by "
                    + Thread.currentThread().getName() + " at epoch:" + System.currentTimeMillis() + " here");
        } else {
            stackTraceHolder = null;
        }
        Core.Priority.FILE_DESCRIPTORS.getContext().register(this);
    }

    void markClosedByNio() {
        closedByNIO = true;
    }

    @Override
    public void beforeCheckpoint(Context<? extends jdk.crac.Resource> context) throws Exception {
        if (!closedByNIO) {
            synchronized (fileDescriptor) {
                if (fileDescriptor.valid()) {
                    applyCheckpointPolicy();
                }
            }
        }
    }

    @SuppressWarnings("fallthrough")
    private void applyCheckpointPolicy() throws CheckpointOpenFileException {
        JDKContext ctx = Core.getJDKContext();
        int fd = fdAccess.get(fileDescriptor);
        String path = getPath(fileDescriptor);
        String type = getType(fileDescriptor);
        OpenFDPolicies.BeforeCheckpoint policy = OpenFDPolicies.CHECKPOINT.get(fd, type, path);
        switch (policy) {
            case ERROR:
                if (ctx.claimFdWeak(fileDescriptor, fileDescriptor)) {
                    String info;
                    if ("socket".equals(type)) {
                        info = Socket.getDescription(fileDescriptor);
                    } else {
                        info = (path != null ? path : "unknown path") + " (" + (type != null ? type : "unknown") + ")";
                    }
                    String msg = "FileDescriptor " + fd + " left open: " + info + " ";
                    if (!JDKContext.Properties.COLLECT_FD_STACKTRACES) {
                        msg += JDKContext.COLLECT_FD_STACKTRACES_HINT;
                    }
                    throw new CheckpointOpenFileException(msg, fileDescriptor.resource.stackTraceHolder);
                }
                break;
            case WARN_CLOSE:
                LoggerContainer.warn("CRaC: File descriptor {0} ({1}) was not closed by the application!", fd, path);
                // intentional fallthrough
            case CLOSE:
                fileDescriptor.resource.originalFd = fd;
                fileDescriptor.resource.originalType = type;
                fileDescriptor.resource.originalPath = path;
                fileDescriptor.resource.originalFlags = getFlags(fileDescriptor);
                fileDescriptor.resource.originalOffset = getOffset(fileDescriptor);
                if (fileDescriptor.resource.originalOffset < 0) {
                    throw new CheckpointOpenFileException("Cannot find current offset of descriptor " + fd + "(" + path + ")", null);
                }
                try {
                    // do not unregister any handlers
                    fdAccess.closeNoCleanup(fileDescriptor);
                } catch (IOException e) {
                    throw new CheckpointOpenFileException("Cannot close file descriptor " + fd + " (" + path + ") before checkpoint", e);
                }
                LoggerContainer.debug("Closed FD {0} ({1}, offset {2} with flags 0x{3}%n",
                        fileDescriptor.resource.originalFd, fileDescriptor.resource.originalPath, fileDescriptor.resource.originalOffset,
                        Integer.toHexString(fileDescriptor.resource.originalFlags).toUpperCase());
                break;
            default:
                throw new IllegalArgumentException("Unknown policy " + policy);
        }
    }

    @Override
    public void afterRestore(Context<? extends jdk.crac.Resource> context) throws Exception {
        synchronized (fileDescriptor) {
            if (!fileDescriptor.valid() && originalFd >= 0) {
                applyRestorePolicy();
            }
            // let GC collect the path and type
            originalPath = null;
            originalType = null;
        }
    }

    private void applyRestorePolicy() throws OpenFDPolicies.RestoreFileDescriptorException {
        OpenFDPolicies.AfterRestorePolicy policy =
                OpenFDPolicies.RESTORE.get(originalFd, originalType, originalPath);
        if (policy.type == OpenFDPolicies.AfterRestore.KEEP_CLOSED) {
            LoggerContainer.debug("FD %d (%s) is not reopened per policy%n",
                    originalFd, originalPath);
            originalPath = null;
            originalType = null;
            return;
        }
        String path;
        if (policy.type == OpenFDPolicies.AfterRestore.OPEN_OTHER ||
                policy.type == OpenFDPolicies.AfterRestore.OPEN_OTHER_AT_END) {
            path = policy.param;
        } else {
            if (originalPath == null) {
                throw new OpenFDPolicies.RestoreFileDescriptorException("Cannot reopen file descriptor " +
                        originalFd + ": invalid path: " + originalPath);
            } else if (originalType.equals("socket")) {
                throw new OpenFDPolicies.RestoreFileDescriptorException("Cannot reopen file descriptor " +
                        originalFd + ": cannot restore socket");
            }
            path = originalPath;
        }
        long offset;
        if (policy.type == OpenFDPolicies.AfterRestore.REOPEN_AT_END ||
                policy.type == OpenFDPolicies.AfterRestore.OPEN_OTHER_AT_END) {
            offset = -1;
        } else {
            // We will attempt to open at the original offset even if the path changed;
            // this is used probably as the file moved on the filesystem but the contents
            // are the same.
            offset = originalOffset;
        }
        if (!reopen(originalFd, path, originalFlags, offset)) {
            if (policy.type == OpenFDPolicies.AfterRestore.REOPEN_OR_NULL) {
                if (!reopenNull(originalFd)) {
                    throw new OpenFDPolicies.RestoreFileDescriptorException("Cannot reopen file descriptor " +
                            originalFd + " to null device");
                }
            } else {
                throw new OpenFDPolicies.RestoreFileDescriptorException("Cannot reopen file descriptor " +
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
        return getClass().getName() + "(FD " + fdAccess.get(fileDescriptor) + ")";
    }

    private static native String getPath(FileDescriptor fd);

    private static native String getType(FileDescriptor fd);

    private static native int getFlags(FileDescriptor fd);

    private static native long getOffset(FileDescriptor fd);

    private static native boolean reopen(int fd, String path, int flags, long offset);

    private static native boolean reopenNull(int fd);
}
