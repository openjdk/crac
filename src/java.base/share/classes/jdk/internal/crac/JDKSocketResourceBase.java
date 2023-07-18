package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.impl.CheckpointOpenSocketException;

import java.io.FileDescriptor;
import java.io.IOException;
import java.net.*;
import java.nio.file.FileSystems;
import java.util.Map;
import java.util.function.Predicate;
import java.util.function.Supplier;

public abstract class JDKSocketResourceBase extends JDKFdResource {
    protected final Object owner;
    private boolean valid;
    private boolean error;

    public JDKSocketResourceBase(Object owner) {
        this.owner = owner;
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
                case "close", "reopen":
                    try {
                        closeBeforeCheckpoint();
                    } catch (IOException e) {
                        throw new CheckpointOpenSocketException("Cannot close " + owner, e);
                    }
                    // intentional fallthrough
                case "ignore":
                    if (Boolean.parseBoolean(policy.params.getOrDefault("warn", "true"))) {
                        LoggerContainer.warn("Socket {0} was not closed by the application. Use 'warn: false' in the policy to suppress this message.", owner);
                    }
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
                // FIXME: implement
                if (action.equals("reopen")) {
                    throw new UnsupportedOperationException("Policy " + policy.type + " not implemented");
                }
            } finally {
                reset();
            }
        }
    }

    protected abstract void reset();
}
