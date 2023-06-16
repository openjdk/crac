package jdk.crac.impl;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKResource;
import jdk.internal.crac.LoggerContainer;

import java.nio.file.FileSystems;
import java.nio.file.Path;
import java.nio.file.PathMatcher;
import java.util.*;

public class OpenFilePolicies<P> {
    public static final String CHECKPOINT_PROPERTY = "jdk.crac.file-policy.checkpoint";
    public static final String RESTORE_PROPERTY = "jdk.crac.file-policy.restore";
    public static final String FIFO = "FIFO";

    public static final OpenFilePolicies<BeforeCheckpoint> CHECKPOINT =
            new OpenFilePolicies<>(BeforeCheckpoint.ERROR);
    public static final OpenFilePolicies<AfterRestorePolicy> RESTORE =
            new OpenFilePolicies<>(new AfterRestorePolicy(AfterRestore.REOPEN_OR_ERROR, null));

    private static final JDKResource resource = new JDKResource() {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            CHECKPOINT.clear();
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
    private final List<Map.Entry<PathMatcher, P>> pathPolicies = new ArrayList<>();
    private P fifoPolicy;

    static {
        // This static ctor runs too early to invoke loadCheckpointPolicies directly
        Core.Priority.NORMAL.getContext().register(resource);
    }

    private synchronized static void loadCheckpointPolicies() {
        PolicyUtils.loadProperties("checkpoint", CHECKPOINT_PROPERTY).forEach((key, value) -> {
            BeforeCheckpoint policy;
            try {
                policy = BeforeCheckpoint.valueOf(value.toString());
            } catch (IllegalArgumentException e) {
                throw new IllegalArgumentException(String.format(
                        "Invalid value of policy %s for target %s; valid values are: %s",
                        value, key, Arrays.toString(BeforeCheckpoint.values())));
            }
            CHECKPOINT.setPolicy(key.toString(), value, policy);
        });
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
            switch (type) {
                case OPEN_OTHER:
                    // we add + 2 because we need the equal sign and at least one character for the path
                    if (pstr.length() < type.name().length() + 2) {
                        throw new IllegalArgumentException(String.format(
                                "Invalid specification for policy %s for target %s: " +
                                "Policy name should be followed by an equal sign '=' and then the path.", type, key));
                    } else {
                        policy = new AfterRestorePolicy(type, PolicyUtils.unescape(pstr, type.name().length() + 1, pstr.length()));
                    }
                    break;
                default:
                    policy = new AfterRestorePolicy(type, null);
            }
            RESTORE.setPolicy(key.toString(), value, policy);
        });
        RESTORE.loaded = true;
    }

    public OpenFilePolicies(P defaultPolicy) {
        this.defaultPolicy = defaultPolicy;
        this.fifoPolicy = defaultPolicy;
    }

    private void clear() {
        this.numericPolicies.clear();
        this.pathPolicies.clear();
        this.loaded = false;
        this.fifoPolicy = defaultPolicy;
    }

    public static void ensureRegistered() {
        // Noop - this method is invoked to ensure that static constructor was invoked
    }

    private void setPolicy(String key, Object value, P policy) {
        if (FIFO.equals(key)) {
            fifoPolicy = policy;
        } else if (PolicyUtils.NUMERIC.matcher(key).matches()) {
            int fd = Integer.parseInt(key);
            P prev = numericPolicies.putIfAbsent(fd, policy);
            if (prev != null) {
                LoggerContainer.error("Duplicate policy for file descriptor {0}; policy {1} will be ignored.", fd, value);
            }
        } else {
            pathPolicies.add(Map.entry(
                    FileSystems.getDefault().getPathMatcher("glob:" + key), policy));
        }
    }

    public P get(int fd, String path, String type) {
        synchronized (OpenFilePolicies.this) {
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
        if ("fifo".equals(type) && fd > 2) {
            return fifoPolicy;
        }
        return defaultPolicy;
    }

    /**
     * Defines a treatment of file found open during checkpoint.
     */
    public enum BeforeCheckpoint {
        /**
         * The checkpoint fails with an appropriate error message. This is the
         * default as it is safer to force applications handle the checkpoint,
         * the options below are meant as workarounds when this is not feasible.
         */
        ERROR,
        /**
         * The file will be silently closed. The original path will
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
         * The file is reopened; if the file was opened in append mode it is
         * opened at the end, otherwise it is opened at the original position.
         * If it cannot be opened (e.g. the file is not on the filesystem
         * anymore or the process has insufficient permissions) an error is
         * printed and the checkpoint throws an exception.
         * When the file does not exist this is treated as an error even if
         * it was previously opened with flags supporting creation.
         * This is the default behaviour.
         */
        REOPEN_OR_ERROR,
        /**
         * The file is reopened. If it cannot be opened (e.g. the file
         * is missing or the process has insufficient permissions) a /dev/null
         * device is opened instead (allowing the process to write any data and
         * immediately returning EOF when reading).
         */
        REOPEN_OR_NULL,
        /**
         * After restore another file specified as part of the policy
         * declaration is opened instead; the policy name should be followed
         * by an equal sign '=' and the new path.
         * If the file was opened in append mode the new file is opened at the end;
         * otherwise it is opened at the same position.
         * The file must be present; a non-existent file is treated as an error.
         * If the other file cannot be open the checkpoint throws an exception.
         */
        OPEN_OTHER,
        /**
         * Do not do anything with the closed file; this will probably result
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
