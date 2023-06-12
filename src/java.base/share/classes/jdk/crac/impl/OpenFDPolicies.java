package jdk.crac.impl;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKResource;
import jdk.internal.crac.LoggerContainer;
import sun.security.action.GetPropertyAction;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileSystems;
import java.nio.file.Path;
import java.nio.file.PathMatcher;
import java.util.*;
import java.util.regex.Pattern;

import static jdk.crac.impl.OpenFDPolicies.AfterRestore.OPEN_OTHER;

public class OpenFDPolicies<P> {
    public static final String CHECKPOINT_PROPERTY = "jdk.crac.fd-policy.checkpoint";
    public static final String RESTORE_PROPERTY = "jdk.crac.fd-policy.restore";
    public static final String FIFO = "FIFO";
    public static final String SOCKET = "SOCKET";

    public static final OpenFDPolicies<BeforeCheckpoint> CHECKPOINT =
            new OpenFDPolicies<>(BeforeCheckpoint.ERROR);
    public static final OpenFDPolicies<AfterRestorePolicy> RESTORE =
            new OpenFDPolicies<>(new AfterRestorePolicy(AfterRestore.REOPEN_OR_ERROR, null));

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
        public void afterRestore(Context<? extends Resource> context) throws Exception {
        }
    };

    private final P defaultPolicy;
    private boolean loaded;
    private P fifoPolicy;
    private P socketPolicy;
    private final Map<Integer, P> numericPolicies = new HashMap<>();
    private final List<Map.Entry<PathMatcher, P>> pathPolicies = new ArrayList<>();

    static {
        // This static ctor runs too early to invoke loadCheckpointPolicies directly
        Core.Priority.NORMAL.getContext().register(resource);
    }

    private synchronized static void loadCheckpointPolicies() {
        // We cannot initialize this in static constructor as this is invoked too early
        if (NUMERIC == null) {
            NUMERIC = Pattern.compile("[0-9]+");
        }
        loadProperties("checkpoint", CHECKPOINT_PROPERTY).forEach((key, value) -> {
            BeforeCheckpoint policy;
            try {
                policy = BeforeCheckpoint.valueOf(value.toString());
            } catch (IllegalArgumentException e) {
                LoggerContainer.error("Invalid value of policy '{0}' for target {1}; valid values are: {2}",
                        value, key, Arrays.toString(BeforeCheckpoint.values()));
                return;
            }
            CHECKPOINT.setPolicy(key, value, policy);
        });
        CHECKPOINT.loaded = true;
    }

    private synchronized static void loadRestorePolicies() {
        AfterRestore[] policies = AfterRestore.values();
        loadProperties("restore", RESTORE_PROPERTY).forEach((key, value) -> {
            String pstr = value.toString();
            AfterRestorePolicy policy = null;
            for (var p : policies) {
                if (pstr.startsWith(p.name())) {
                    if (p == OPEN_OTHER) {
                        // we add + 2 because we need the equal sign and at least one character for the path
                        if (pstr.length() < OPEN_OTHER.name().length() + 2 || pstr.charAt(OPEN_OTHER.name().length()) != '=') {
                            LoggerContainer.error("Invalid specification for policy '{0}' for target {1}: " +
                                    "Policy name should be followed by an equal sign '=' and then the path.", OPEN_OTHER, key);
                            return;
                        } else {
                            policy = new AfterRestorePolicy(OPEN_OTHER,
                                    unescape(pstr, OPEN_OTHER.name().length() + 1, pstr.length()));
                        }
                    } else if (pstr.length() == p.name().length()) {
                        policy = new AfterRestorePolicy(p, null);
                    }
                }
            }
            if (policy == null) {
                LoggerContainer.error("Invalid value of restore policy '{0}' for target {1}; valid values are: {2}",
                        value, key, Arrays.toString(AfterRestore.values()));
            } else {
                RESTORE.setPolicy(key, value, policy);
            }
        });
        RESTORE.loaded = true;
    }

    public OpenFDPolicies(P defaultPolicy) {
        this.defaultPolicy = defaultPolicy;
        this.fifoPolicy = defaultPolicy;
        this.socketPolicy = defaultPolicy;
    }

    private void clear() {
        this.fifoPolicy = defaultPolicy;
        this.socketPolicy = defaultPolicy;
        this.numericPolicies.clear();
        this.pathPolicies.clear();
        this.loaded = false;
    }

    private static Properties loadProperties(String type, String systemProperty) {
        Properties properties = new Properties();
        String file = GetPropertyAction.privilegedGetProperty(systemProperty + ".file");
        if (file != null) {
            try {
                if (file.length() >= 4 && file.substring(file.length() - 4).equalsIgnoreCase(".xml")) {
                    try (var fis = new FileInputStream(file)) {
                        properties.loadFromXML(fis);
                    }
                } else {
                    try (var fr = new FileReader(file, StandardCharsets.UTF_8)) {
                        properties.load(fr);
                    }
                }
            } catch (IOException e) {
                LoggerContainer.error("Failed to read {0} file descriptor policies from {1}: {2}", type, file, e.getMessage());
            }
        }
        String property = GetPropertyAction.privilegedGetProperty(systemProperty);
        if (property != null) {
            for (var item : property.split(File.pathSeparator)) {
                int eqIndex = findNonEscapedEq(item, 0);
                if (eqIndex < 0) {
                    LoggerContainer.error("Invalid specification for {0} file descriptor policy: {1}", type, item);
                } else {
                    properties.put(unescape(item, 0, eqIndex), item.substring(eqIndex + 1));
                }
            }
        }
        return properties;
    }

    private static String unescape(String str, int fromIndex, int toIndex) {
        boolean escaped = false;
        StringBuilder sb = new StringBuilder(str.length() - fromIndex);
        for (int i = fromIndex; i < toIndex; ++i) {
            char c = str.charAt(i);
            if (!escaped && c == '\\') {
                escaped = true;
            } else {
                sb.append(c);
                escaped = false;
            }
        }
        return sb.toString();
    }

    private static int findNonEscapedEq(String str, int fromIndex) {
        boolean escaped = false;
        for (int i = fromIndex; i < str.length(); ++i) {
            char c = str.charAt(i);
            if (c == '\\') {
                escaped = !escaped;
            } else if (c == '=' && !escaped) {
                return i;
            }
        }
        return -1;
    }

    public static void ensureRegistered() {
        // Noop - this method is invoked to ensure that static constructor was invoked
    }

    private void setPolicy(Object key, Object value, P policy) {
        if (FIFO.equals(key)) {
            fifoPolicy = policy;
        } else if (SOCKET.equals(key)) {
            socketPolicy = policy;
        } else if (NUMERIC.matcher(key.toString()).matches()) {
            int fd = Integer.parseInt(key.toString());
            P prev = numericPolicies.putIfAbsent(fd, policy);
            if (prev != null) {
                LoggerContainer.error("Duplicate policy for file descriptor {0}; policy {1} will be ignored.", fd, value);
            }
        } else {
            pathPolicies.add(Map.entry(
                    FileSystems.getDefault().getPathMatcher("glob:" + key), policy));
        }
    }

    public P get(int fd, String type, String path) {
        synchronized (OpenFDPolicies.this) {
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
        if (path != null) {
            Path p = Path.of(path);
            for (var entry : pathPolicies) {
                if (entry.getKey().matches(p)) {
                    return entry.getValue();
                }
            }
        }
        if (type.equals("fifo") && fd > 2) {
            return fifoPolicy;
        } else if (type.equals("socket")) {
            return socketPolicy;
        }
        return defaultPolicy;
    }

    /**
     * Defines a treatment of file descriptor found open during checkpoint.
     */
    public enum BeforeCheckpoint {
        /**
         * The checkpoint fails with an appropriate error message. This is the
         * default as it is safer to force applications handle the checkpoint,
         * the options below are meant as workarounds when this is not feasible.
         */
        ERROR,
        /**
         * The file descriptor will be silently closed. The original path will
         * be recorded and after restore this will be subject to treatment based
         * on the {@link AfterRestore} policy.
         */
        CLOSE,
        /**
         * The behaviour is identical to {@link #CLOSE} but the application
         * will print out a warning message to the standard error.
         */
        WARN_CLOSE,
        // TODO: for no-downtime replication (scaling up) a strategy that would
        //  keep the descriptor open when the checkpointed process is left
        //  running might be useful.
    }

    /**
     * Defines what to do with file descriptors closed by the
     * {@link BeforeCheckpoint} policy after restore from a checkpoint.
     */
    public enum AfterRestore {
        /**
         * The file descriptor is reopened. If it cannot be opened (e.g.
         * the file is not on the filesystem anymore or the process has
         * insufficient permissions) an error is printed and the restored
         * process is terminated.
         * If the file descriptor had no matching file (it is a pipe or
         * socket) this is treated as an error.
         * This is the default behaviour.
         */
        REOPEN_OR_ERROR,
        /**
         * The file descriptor is reopened. If it cannot be opened (e.g.
         * the file is missing, it was a pipe or socket or the process has
         * insufficient permissions) a /dev/null device is opened instead
         * (allowing the process to write any data but not providing anything
         * for reading).
         */
        REOPEN_OR_NULL,
        /**
         * After restore another file specified as part of the policy
         * declaration (usually the policy name is followed by
         * an equal sign '=' character and then the path) is opened instead.
         * If the other file cannot be open an error is printed and the process
         * is terminated.
         */
        OPEN_OTHER,
        /**
         * Do not do anything with the closed descriptor; this will probably result
         * in runtime errors if the resource is used.
         */
        KEEP_CLOSED
    }

    public static class AfterRestorePolicy {
        public final AfterRestore type;
        public final String param;

        private AfterRestorePolicy(AfterRestore type, String param) {
            this.type = type;
            this.param = param;
        }
    }

    public static class RestoreFileDescriptorException extends Exception {
        public static final long serialVersionUID = -8790346029973354266L;

        public RestoreFileDescriptorException(String message) {
            super(message);
        }
    }
}
