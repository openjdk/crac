package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;
import sun.security.action.GetPropertyAction;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.*;
import java.util.function.Predicate;

public class OpenResourcePolicies {
    public static final String PROPERTY = "jdk.crac.resource-policies";
    public static final String FILE = "file";
    public static final String PIPE = "pipe";
    public static final String SOCKET = "socket";

    private enum State {
        NOT_LOADED,
        LOADED_FOR_CHECKPOINT,
        LOADED_FOR_RESTORE
    }

    private static final Map<String, List<Policy>> policies = new HashMap<>();
    private static State state = State.NOT_LOADED;

    private static final JDKResource resource = new JDKResource() {
        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            // We cannot lazily wait until the FILE_DESCRIPTORS priority
            // because we need to open a file, too.
            loadPolicies(false);
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
            policies.clear();
            state = State.NOT_LOADED;
        }
    };

    static {
        Core.Priority.NORMAL.getContext().register(resource);
    }

    static void ensureRegistered() {
        // noop
    }

    private static synchronized void loadPolicies(boolean isRestore) {
        if (state == State.LOADED_FOR_RESTORE || (!isRestore && state == State.LOADED_FOR_CHECKPOINT)) {
            return;
        }
        // prevent loading recursively
        state = isRestore ? State.LOADED_FOR_RESTORE : State.LOADED_FOR_CHECKPOINT;

        String file = GetPropertyAction.privilegedGetProperty(PROPERTY);
        if (file == null) {
            return;
        }
        // The newer policies have more priority; we'll copy the old ones and
        // append them later on
        Map<String, List<Policy>> old = Map.copyOf(policies);
        policies.clear();

        File f = new File(file);
        if (!f.exists()) {
            throw new ConfigurationException("File " + file + " used in property " + PROPERTY + " does not exist");
        } else if (!f.isFile() || !f.canRead()) {
            throw new ConfigurationException("File " + file + " used in property " + PROPERTY + " is not a regular file or cannot be read.");
        }
        String type = null, action = null;
        Map<String, String> params = new HashMap<>();
        int currentLine = 1, policyStart = 1;
        try {
            for (String line : Files.readAllLines(f.toPath())) {
                line = line.trim();
                if (line.startsWith("#")) {
                    continue;
                } else if ("---".equals(line)) {
                    if (type == null && action == null && params.isEmpty()) {
                        // ignore empty policies
                        policyStart = currentLine + 1;
                        continue;
                    }
                    addPolicy(type, action, params, file, policyStart, currentLine);
                    type = null;
                    action = null;
                    params = new HashMap<>();
                    policyStart = currentLine + 1;
                    continue;
                }
                int index = line.indexOf(": ");
                if (index < 0) {
                    throw new ConfigurationException(invalid(file, policyStart, currentLine, "cannot parse line " + currentLine + ": " + line));
                }
                String key = line.substring(0, index).trim();
                String value = line.substring(index + 2).trim();
                switch (key.toLowerCase()) {
                    case "type" -> type = value;
                    case "action" -> action = value;
                    default -> params.put(key, value);
                }
                ++currentLine;
            }
            if (type != null || action != null || !params.isEmpty()) {
                addPolicy(type, action, params, file, policyStart, currentLine);
            }
        } catch (IOException e) {
            throw new ConfigurationException("Cannot read file " + file + " used in property " + PROPERTY, e);
        }

        // Add the old policies after the newly loaded ones
        for (var entry : old.entrySet()) {
            List<Policy> newList = policies.get(entry.getKey());
            if (newList == null) {
                policies.put(entry.getKey(), entry.getValue());
            } else {
                newList.addAll(entry.getValue());
            }
        }
    }

    private static void addPolicy(String type, String action, Map<String, String> params, String file, int from, int to) {
        if (type == null) {
            throw new ConfigurationException(invalid(file, from, to, "no 'type'"));
        } else if (action == null) {
            throw new ConfigurationException(invalid(file, from, to, "no 'action'"));
        }
        type = type.trim().toLowerCase();
        action = action.trim();
        policies.computeIfAbsent(type, t -> new ArrayList<>()).add(new Policy(type, action, params));
    }

    private static String invalid(String file, int from, int to, String why) {
        return "Invalid rule in policies file " + file + " on lines " + from + "-" + to + ": " + why;
    }

    public static Policy find(boolean isRestore, String type, Predicate<Map<String, String>> filter) {
        loadPolicies(isRestore);
        List<Policy> list = policies.get(type);
        if (list == null) {
            return null;
        }
        if (filter == null) {
            return list.get(0);
        }
        return list.stream().filter(p -> filter.test(p.params)).findFirst().orElse(null);
    }

    public static class Policy {
        // file, socket, pipe...
        public final String type;
        // The policy action
        public final String action;
        // Both filtering and action customization
        public final Map<String, String> params;

        public Policy(String type, String action, Map<String, String> params) {
            this.type = type;
            this.action = action;
            this.params = Collections.unmodifiableMap(params);
        }
    }

    private static class ConfigurationException extends RuntimeException {
        private static final long serialVersionUID = 6833568262773571378L;

        public ConfigurationException(String message) {
            super(message);
        }

        public ConfigurationException(String message, Throwable cause) {
            super(message, cause);
        }
    }
}
