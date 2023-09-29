package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.impl.CheckpointOpenFileException;
import jdk.crac.impl.CheckpointOpenResourceException;
import sun.security.action.GetPropertyAction;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Supplier;

public abstract class JDKFileResource extends JDKFdResource {
    private static final Path[] CLASSPATH_ENTRIES;

    static {
        String[] items = GetPropertyAction.privilegedGetProperty("java.class.path")
                .split(File.pathSeparator);
        CLASSPATH_ENTRIES = new Path[items.length];
        for (int i = 0; i < items.length; i++) {
            CLASSPATH_ENTRIES[i] = Path.of(items[i]);
        }
    }

    boolean closed;
    boolean error;

    public static OpenResourcePolicies.Policy findPolicy(boolean isRestore, String pathStr) {
        Path path = Path.of(pathStr);
        return OpenResourcePolicies.find(isRestore,
                OpenResourcePolicies.FILE, props -> {
                    String policyPath = props.get("path");
                    if (policyPath == null) {
                        return true; // missing path matches all files
                    } else {
                        return FileSystems.getDefault().getPathMatcher("glob:" + policyPath).matches(path);
                    }
                });
    }

    protected abstract FileDescriptor getFD();
    protected abstract String getPath();
    protected abstract void closeBeforeCheckpoint(OpenResourcePolicies.Policy policy) throws IOException;
    protected abstract void reopenAfterRestore(OpenResourcePolicies.Policy policy) throws IOException;

    protected boolean matchClasspath(String path) {
        Path p = Path.of(path);
        for (Path entry : CLASSPATH_ENTRIES) {
            try {
                if (Files.isSameFile(p, entry)) {
                    return true;
                }
            } catch (IOException e) {
                // ignored
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

        OpenResourcePolicies.Policy policy = findPolicy(false, path);
        String action = "error";
        if (policy != null) {
            action = policy.action.toLowerCase();
        } else if (matchClasspath(path)) {
            // Files on the classpath are considered persistent, exception is not thrown
            action = "ignore";
        }
        Supplier<Exception> exceptionSupplier = switch (action) {
            case "error":
                error = true;
                yield () -> new CheckpointOpenFileException(path, getStackTraceHolder());
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
                warnOpenResource(policy, "File " + path);
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
        OpenResourcePolicies.Policy policy = findPolicy(true, getPath());
        if (policy != null && "reopen".equalsIgnoreCase(policy.action)) {
            reopenAfterRestore(policy);
            closed = false;
        }
    }
}
