package jdk.crac.impl;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKResource;
import jdk.internal.crac.LoggerContainer;

import java.net.*;
import java.util.*;
import java.util.regex.Pattern;

import static jdk.crac.impl.PolicyUtils.unescape;

public class OpenSocketPolicies<P> {
    public static final String CHECKPOINT_PROPERTY = "jdk.crac.socket-policy.checkpoint";
    public static final String RESTORE_PROPERTY = "jdk.crac.socket-policy.restore";

    public static final OpenSocketPolicies<BeforeCheckpoint> CHECKPOINT =
            new OpenSocketPolicies<>(BeforeCheckpoint.ERROR);
    public static final OpenSocketPolicies<AfterRestorePolicy> RESTORE =
            new OpenSocketPolicies<>(new AfterRestorePolicy(AfterRestore.REOPEN_OR_ERROR, null, null));

    private static Pattern NUMERIC;
    private static final JDKResource resource = new JDKResource() {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            CHECKPOINT.clear();
            // We need to s
            loadCheckpointPolicies();
            RESTORE.clear();
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
        }
    };

    private final P defaultPolicy;
    private boolean loaded;
    private final Map<Integer, P> numericPolicies = new HashMap<>();
    private final List<AddressPolicy<P>> addressPolicies = new ArrayList<>();

    static {
        // This static ctor runs too early to invoke loadCheckpointPolicies directly
        Core.Priority.NORMAL.getContext().register(resource);
    }

    private synchronized static void loadCheckpointPolicies() {
        // We cannot initialize this in static constructor as this is invoked too early
        if (NUMERIC == null) {
            NUMERIC = Pattern.compile("[0-9]+");
        }
        PolicyUtils.loadProperties("checkpoint", CHECKPOINT_PROPERTY).forEach((key, value) -> {
            BeforeCheckpoint policy;
            try {
                policy = BeforeCheckpoint.valueOf(value.toString());
            } catch (IllegalArgumentException e) {
                LoggerContainer.error("Invalid value of policy '{0}' for target {1}; valid values are: {2}",
                        value, key, Arrays.toString(BeforeCheckpoint.values()));
                return;
            }
            CHECKPOINT.setPolicy(key.toString(), value, policy);
        });
        CHECKPOINT.addressPolicies.sort(Comparator.comparing(AddressPolicy::priority));
        CHECKPOINT.loaded = true;
    }

    private synchronized static void loadRestorePolicies() {
        AfterRestore[] policies = AfterRestore.values();
        PolicyUtils.loadProperties("restore", RESTORE_PROPERTY).forEach((key, value) -> {
            String pstr = value.toString();
            int eqIndex = pstr.indexOf('=');
            String policyName = eqIndex < 0 ? pstr : pstr.substring(0, eqIndex);
            AfterRestore type = Arrays.stream(policies).filter(p -> policyName.equals(p.name())).findAny().orElse(null);
            if (type == null) {
                throw new IllegalArgumentException(String.format(
                        "Invalid value of restore policy %s for target %s; valid values are: %s",
                        value, key, Arrays.toString(AfterRestore.values())));
            }
            AfterRestorePolicy policy;
            if (type == AfterRestore.OPEN_OTHER) {
                // we add + 2 because we need the equal sign and at least one character for the path
                if (pstr.length() < type.name().length() + 2) {
                    throw new IllegalArgumentException(String.format(
                            "Invalid specification for policy %s for target %s: " +
                            "Policy name should be followed by an equal sign '=' and then the local,remote addresses.",
                            type, key));
                } else {
                    // The address in key (for matching) got unescaped in loadProperties, so in general
                    // the addresses must not contain comma anyway, but we'll try anyway.
                    int begin = type.name().length() + 1;
                    int comma = PolicyUtils.findNonEscaped(pstr, begin, ',');
                    SocketAddress local, remote = null;
                    if (comma < 0) {
                        local = parseAddress(unescape(pstr, begin, pstr.length()));
                    } else {
                        local = parseAddress(unescape(pstr, begin, comma));
                        remote = parseAddress(unescape(pstr, comma + 1, pstr.length()));
                    }
                    policy = new AfterRestorePolicy(type, local, remote);
                }
            } else {
                policy = new AfterRestorePolicy(type, null, null);
            }
            RESTORE.setPolicy(key.toString(), value, policy);
        });
        RESTORE.loaded = true;
    }

    public OpenSocketPolicies(P defaultPolicy) {
        this.defaultPolicy = defaultPolicy;
    }

    private void clear() {
        this.numericPolicies.clear();
        this.addressPolicies.clear();
        this.loaded = false;
    }

    public static void ensureRegistered() {
        // Noop - this method is invoked to ensure that static constructor was invoked
    }

    private void setPolicy(String key, Object value, P policy) {
        if (NUMERIC.matcher(key).matches()) {
            int fd = Integer.parseInt(key);
            P prev = numericPolicies.putIfAbsent(fd, policy);
            if (prev != null) {
                LoggerContainer.error("Duplicate policy for file descriptor {0}; policy {1} will be ignored.", fd, value);
            }
        } else {
            int comma = key.indexOf(',');
            String localStr = key, remoteStr = null;
            if (comma >= 0) {
                localStr = key.substring(0, comma);
                remoteStr = key.substring(comma + 1);
            }
            SocketAddress local = parseAddress(localStr);
            SocketAddress remote = parseAddress(remoteStr);
            addressPolicies.add(new AddressPolicy<>(local, remote, policy));
        }
    }

    // This will return UnixDomainSocketAddress when we can't parse it
    // which will result in no match later on
    private static SocketAddress parseAddress(String str) {
        if (str == null || str.isBlank()) {
            return null;
        }
        str = str.trim();
        if ("*".equals(str)) {
            return null;
        }
        int colonIndex = str.lastIndexOf(':');
        if (colonIndex < 0) {
            // unix path or only IP (wildcard port)
            try {
                return new InetSocketAddress(InetAddress.getByName(str), 0);
            } catch (UnknownHostException e) {
                return UnixDomainSocketAddress.of(str);
            }
        } else {
            try {
                String portPart = str.substring(colonIndex + 1).trim();
                int port;
                if (portPart.isEmpty() || "*".equals(portPart)) {
                    port = 0;
                } else {
                    port = Integer.parseInt(portPart);
                    if (port < 0 || port > 0xFFFF) {
                        // probably malformed address?
                        return UnixDomainSocketAddress.of(str);
                    }
                }
                String addressPart = str.substring(0, colonIndex).trim();
                if (addressPart.isEmpty() || "*".equals(addressPart)) {
                    if (port == 0) {
                        return null;
                    } else {
                        return new WildcardInetAddress(port);
                    }
                }
                return new InetSocketAddress(InetAddress.getByName(addressPart), port);
            } catch (NumberFormatException ignored) {
                // doesn't look like a port -> unix domain socket to C:\ or something like that?
            } catch (UnknownHostException e) {
                // address cannot be resolved
            }
        }
        return UnixDomainSocketAddress.of(str);
    }

    public P get(int fd, SocketAddress local, SocketAddress remote) {
        synchronized (OpenSocketPolicies.this) {
            if (!loaded) {
                // We could use a Runnable but method references don't work
                // when the static ctor is invoked
                if (this == CHECKPOINT) {
                    loadCheckpointPolicies();
                } else {
                    loadRestorePolicies();
                }
            }
        }
        P policy = numericPolicies.get(fd);
        if (policy != null) {
            return policy;
        }
        for (var addressPolicy : addressPolicies) {
            LoggerContainer.error("test {0} = {1} && {2} = {3}", addressPolicy.local, local, addressPolicy.remote, remote);
            if (matches(addressPolicy.local, local) && matches(addressPolicy.remote, remote)) {
                return addressPolicy.policy;
            }
        }
        return defaultPolicy;
    }

    private boolean matches(SocketAddress config, SocketAddress actual) {
        // Note: null local address as parameter (unbound) does match only to wildcard (null)
        if (config == null || config.equals(actual)) {
            return true;
        }
        if (config instanceof InetSocketAddress && actual instanceof InetSocketAddress) {
            InetAddress cfgAddress = ((InetSocketAddress) config).getAddress();
            return ((InetSocketAddress) config).getPort() == 0 && cfgAddress.equals(((InetSocketAddress) actual).getAddress());
        } else if (config instanceof WildcardInetAddress && actual instanceof InetSocketAddress) {
            return ((WildcardInetAddress) config).port == ((InetSocketAddress) actual).getPort();
        }
        return false;
    }

    /**
     * Defines a treatment of socket found open during checkpoint.
     */
    public enum BeforeCheckpoint {
        /**
         * The checkpoint fails with an appropriate error message. This is the
         * default as it is safer to force applications handle the checkpoint,
         * the options below are meant as workarounds when this is not feasible.
         */
        ERROR,
        /**
         * The socket will be silently closed. After restore this socket will
         * be subject to treatment based on the {@link AfterRestore} policy.
         */
        CLOSE,
        /**
         * The behaviour is identical to {@link #CLOSE} but the application
         * will print out a warning message to the standard error.
         */
        WARN_CLOSE,
        // TODO: for no-downtime replication (scaling up) a strategy that would
        //  keep the socket open when the checkpointed process is left
        //  running might be useful.
    }

    /**
     * Defines what to do with the sockets closed by the
     * {@link BeforeCheckpoint} policy after restore from a checkpoint.
     */
    public enum AfterRestore {
        /**
         * The socket is reopened. If it cannot be opened (e.g. it was not
         * a connection-less socket or the IP it was bound to is not present)
         * an error is printed and the checkpoint throws an exception.
         * This is the default behaviour.
         */
        REOPEN_OR_ERROR,
        /**
         * After restore the socket is reopened but both the local and remote
         * address can be changed. These are specified as part of the policy
         * declaration: the policy name should be followed by an equal sign '=',
         * new local address, comma, and new remote address. The remote address
         * (and preceding comma) are optional.
         */
        OPEN_OTHER,
        /**
         * Do not do anything with the closed socket; this will probably result
         * in runtime errors if the resource is used.
         */
        KEEP_CLOSED
    }

    public static class AfterRestorePolicy {
        public final AfterRestore type;
        public final SocketAddress local;
        public final SocketAddress remote;

        private AfterRestorePolicy(AfterRestore type, SocketAddress local, SocketAddress remote) {
            this.type = type;
            this.local = local;
            this.remote = remote;
        }
    }

    public static class RestoreFileDescriptorException extends Exception {
        public static final long serialVersionUID = -8790346029973354266L;

        public RestoreFileDescriptorException(String message) {
            super(message);
        }
    }

    private static class AddressPolicy<P> {
        final SocketAddress local;
        final SocketAddress remote;
        final P policy;

        public AddressPolicy(SocketAddress local, SocketAddress remote, P policy) {
            this.local = local;
            this.remote = remote;
            this.policy = policy;
        }

        // for sorting from the most specific to the least specific
        public int priority() {
            if (local != null && remote != null) {
                if (!hasWildcardPort(local) && !hasWildcardPort(remote)) {
                    return 0;
                } else if (!hasWildcardPort(remote)) {
                    return 1;
                } else if (!hasWildcardPort(local)) {
                    return 2;
                } else {
                    return 3;
                }
            } else if (remote != null) {
                return hasWildcardPort(remote) ? 5 : 4;
            } else if (local != null) {
                return hasWildcardPort(local) ? 7 : 6;
            } else {
                return 8;
            }
        }
    }

    private static boolean hasWildcardPort(SocketAddress addr) {
        return addr instanceof InetSocketAddress && ((InetSocketAddress) addr).getPort() == 0;
    }

    private static class WildcardInetAddress extends SocketAddress {
        public static final long serialVersionUID = 8541345859974104981L;

        final int port;

        private WildcardInetAddress(int port) {
            this.port = port;
        }
    }
}
