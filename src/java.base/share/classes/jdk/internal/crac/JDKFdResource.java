package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;
import sun.security.action.GetBooleanAction;

import java.util.function.Supplier;

public abstract class JDKFdResource implements JDKResource {
    public static final String COLLECT_FD_STACKTRACES_PROPERTY = "jdk.crac.collect-fd-stacktraces";
    private static final String COLLECT_FD_STACKTRACES_HINT =
        "Use -D" + COLLECT_FD_STACKTRACES_PROPERTY + "=true to find the source.";

    private static final boolean COLLECT_FD_STACKTRACES =
        GetBooleanAction.privilegedGetProperty(COLLECT_FD_STACKTRACES_PROPERTY);

    // No lambdas during clinit...
    protected static Supplier<Exception> NO_EXCEPTION = new Supplier<Exception>() {
        @Override
        public Exception get() {
            return null;
        }
    };

    final Exception stackTraceHolder;

    static volatile boolean stacktraceHintPrinted = false;
    static volatile boolean warningSuppressionHintPrinted = false;

    public JDKFdResource() {
        stackTraceHolder = COLLECT_FD_STACKTRACES ?
            // About the timestamp: we cannot format it nicely since this
            // exception is sometimes created too early in the VM lifecycle
            // (but it's hard to detect when it would be safe to do).
            new Exception("This file descriptor was created by " + Thread.currentThread().getName()
                + " at epoch:" + System.currentTimeMillis() + " here") :
            null;

        Core.Priority.FILE_DESCRIPTORS.getContext().register(this);
        OpenResourcePolicies.ensureRegistered();
    }

    protected Exception getStackTraceHolder() {
        if (!stacktraceHintPrinted && stackTraceHolder == null) {
            stacktraceHintPrinted = true;
            LoggerContainer.info(COLLECT_FD_STACKTRACES_HINT);
        }
        return stackTraceHolder;
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {

    }

    protected void warnOpenResource(OpenResourcePolicies.Policy policy, String self) {
        // The warning is not printed for implicitly closed resource (without policy)
        // e.g. standard input/output streams
        String warn = "false";
        if (policy != null) {
            warn = policy.params.getOrDefault("warn", "true");
        }
        if (Boolean.parseBoolean(warn)) {
            LoggerContainer.warn("{0} was not closed by the application.", self);
            if (!warningSuppressionHintPrinted) {
                LoggerContainer.info("To suppress the warning above use 'warn: false' in the resource policy.");
                warningSuppressionHintPrinted = true;
            }
        }
    }
}
