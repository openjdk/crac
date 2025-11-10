package jdk.internal.crac;

import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;
import jdk.internal.crac.mirror.impl.CheckpointOpenSocketException;

import java.io.FileDescriptor;
import java.io.IOException;
import java.net.*;
import java.nio.file.FileSystems;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Consumer;
import java.util.function.Predicate;
import java.util.function.Supplier;

public abstract class JDKSocketResourceBase extends JDKFdResource {
    // While the collection should be used by the single thread invoking the checkpoint
    // we won't prevent some races that could touch these as well.
    private static final Map<Object, List<Runnable>> markedForReopen = new ConcurrentHashMap<>();

    protected final Object owner;
    private boolean valid;
    private boolean error;

    public JDKSocketResourceBase(Object owner) {
        super(Core.Priority.SOCKETS);
        this.owner = owner;
    }

    protected static void markForReopen(Object owner) {
        var prev = markedForReopen.putIfAbsent(owner, Collections.synchronizedList(new ArrayList<>()));
        if (prev != null) {
            throw new IllegalStateException("Marking for reopen twice");
        }
    }

    public static Consumer<Runnable> reopenQueue(Object owner) {
        List<Runnable> queue = markedForReopen.get(owner);
        return queue == null ? null : queue::add;
    }

    protected abstract FileDescriptor getFD();
    protected abstract void closeBeforeCheckpoint() throws IOException;
    protected abstract OpenResourcePolicies.Policy findPolicy(boolean isRestore) throws CheckpointOpenSocketException;

    @SuppressWarnings("fallthrough")
    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        FileDescriptor fd = getFD();
        if (fd == null) {
            return;
        }
        synchronized (fd) {
            if (!(valid = fd.valid())) {
                return;
            }
            OpenResourcePolicies.Policy policy = findPolicy(false);
            String action = policy == null ? "error" : policy.action.toLowerCase();
            Supplier<Exception> exceptionSupplier = switch (action) {
                case "error":
                    error = true;
                    yield () -> new CheckpointOpenSocketException(owner.toString(), getStackTraceHolder());
                case "reopen":
                    markForReopen();
                    // intentional fallthrough
                case "close":
                    try {
                        closeBeforeCheckpoint();
                    } catch (IOException e) {
                        throw new CheckpointOpenSocketException("Cannot close " + owner, e);
                    }
                    // intentional fallthrough
                case "ignore":
                    warnOpenResource(policy, "Socket " + owner);
                    yield NO_EXCEPTION;
                default:
                    throw new IllegalStateException("Unknown policy action " + action + " for " + owner, null);
            };
            Core.getClaimedFDs().claimFd(fd, owner, exceptionSupplier, fd);
        }
    }

    protected Predicate<Map<String, String>> getMatcher(SocketAddress addr, String addressKey, String portKey, String pathKey) {
        return params -> {
            String family = params.get("family");
            if (family != null && addr != null) {
                switch (family.toLowerCase()) {
                    case "ipv6", "inet6" -> {
                        if (!(addr instanceof InetSocketAddress inetAddr)) {
                            return false;
                        } else if (!(inetAddr.getAddress() instanceof Inet6Address)) {
                            return false;
                        }
                    }
                    case "ipv4", "inet4" -> {
                        if (!(addr instanceof InetSocketAddress inetAddr)) {
                            return false;
                        } else if (!(inetAddr.getAddress() instanceof Inet4Address)) {
                            return false;
                        }
                    }
                    case "ip", "inet" -> {
                        if (!(addr instanceof InetSocketAddress inetAddr)) {
                            return false;
                        }
                    }
                    case "unix" -> {
                        if (!(addr instanceof UnixDomainSocketAddress)) {
                            return false;
                        }
                    }
                    default -> throw new IllegalArgumentException("Unknown family: " + family);
                }
            }
            String cfgAddress = params.get(addressKey);
            String cfgPort = params.get(portKey);
            String cfgPath = params.get(pathKey);
            if (cfgAddress != null || cfgPort != null) {
                if (!(addr instanceof InetSocketAddress inetAddr)) {
                    return false;
                }
                if (cfgAddress != null && !"*".equals(cfgAddress)) {
                    try {
                        if (!InetAddress.getByName(cfgAddress).equals(inetAddr.getAddress())) {
                            return false;
                        }
                    } catch (UnknownHostException e) {
                        return false;
                    }
                }
                if (cfgPort != null && !"*".equals(cfgPort)) {
                    return Integer.parseInt(cfgPort) == inetAddr.getPort();
                }
                return true;
            } else if (cfgPath != null) {
                if (!(addr instanceof UnixDomainSocketAddress unixAddr)) {
                    return false;
                }
                return FileSystems.getDefault().getPathMatcher("glob:" + cfgPath)
                        .matches(unixAddr.getPath());
            } else {
                return true;
            }
        };
    }

    @SuppressWarnings("fallthrough")
    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        // Don't do anything when we've already failed
        if (!valid || error) {
            return;
        }
        FileDescriptor fd = getFD();
        if (fd == null) {
            return;
        }
        synchronized (fd) {
            OpenResourcePolicies.Policy policy = findPolicy(true);
            String action = policy == null ? "error" : policy.action;
            try {
                if (action.equals("reopen")) {
                    reopenAfterRestore();
                }
            } finally {
                reset();
            }
        }
    }

    protected abstract void reset();

    protected void markForReopen() {
        throw new UnsupportedOperationException("Reopen not implemented on sockets");
    }

    protected void reopenAfterRestore() throws IOException {
        throw new UnsupportedOperationException("Reopen not implemented on sockets");
    }

    protected void afterReopen(Object self) {
        List<Runnable> runnables = markedForReopen.remove(self);
        if (runnables == null) {
            throw new IllegalStateException(self + " was not marked for reopen");
        }
        runnables.forEach(Runnable::run);
    }
}
