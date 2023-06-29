package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.impl.CheckpointOpenFileException;
import jdk.crac.impl.CheckpointOpenResourceException;
import sun.security.action.GetPropertyAction;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.util.function.Supplier;

public abstract class JDKFileResource extends JDKFdResource {
    private static final String[] CLASSPATH_ENTRIES =
        GetPropertyAction.privilegedGetProperty("java.class.path")
            .split(File.pathSeparator);

    boolean closed;
    boolean error;

    protected abstract FileDescriptor getFD();
    protected abstract String getPath();
    protected abstract void closeBeforeCheckpoint(OpenResourcePolicies.Policy policy) throws IOException;
    protected abstract void reopenAfterRestore(OpenResourcePolicies.Policy policy) throws IOException;

    protected boolean matchClasspath(String path) {
        for (String cp : CLASSPATH_ENTRIES) {
            if (cp.equals(path)) {
                return true;
            }
        }
        return false;
    }

    @SuppressWarnings("fallthrough")
    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        String path = getPath();
        if (path == null) {
            // let FileDescriptor claim everything
            return;
        }

        OpenResourcePolicies.Policy policy = OpenResourcePolicies.findForPath(false, path);
        String action = policy != null ? policy.action.toLowerCase() : "error";
        Supplier<Exception> exceptionSupplier = switch (action) {
            case "error":
                error = true;
                // Files on the classpath are considered persistent, exception is not thrown
                yield matchClasspath(path) ? NO_EXCEPTION :
                        () -> new CheckpointOpenFileException(path, getStackTraceHolder());
            case "close", "reopen":
                // Here we assume that the stream is idle; any concurrent access
                // will end with exceptions as the file-descriptors is invalidated
                try {
                    closeBeforeCheckpoint(policy);
                } catch (IOException e) {
                    throw new CheckpointOpenResourceException("Cannot close " + path, e);
                }
                closed = true;
            case "ignore":
                if (Boolean.parseBoolean(policy.params.getOrDefault("warn", "false"))) {
                    LoggerContainer.warn("CRaC: File {0} was not closed by the application!", path);
                }
                yield NO_EXCEPTION;
            default:
                throw new IllegalStateException("Unknown policy action for path " + path + ": " + policy.action);
        };
        FileDescriptor fd = getFD();
        Core.getClaimedFDs().claimFd(fd, this, exceptionSupplier, fd);
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        if (!closed || error) {
            return;
        }
        OpenResourcePolicies.Policy policy = OpenResourcePolicies.findForPath(true, getPath());
        if (policy != null && "reopen".equalsIgnoreCase(policy.action)) {
            reopenAfterRestore(policy);
            closed = false;
        }
    }
}
