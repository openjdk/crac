package java.util.concurrent;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKResource;

final class CpuCount {
    private static int NCPU = Runtime.getRuntime().availableProcessors();
    private static final JDKResource RESOURCE = new JDKResource() {
        @Override
        public Priority getPriority() {
            return Priority.NORMAL;
        }

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) throws Exception {
            NCPU = Runtime.getRuntime().availableProcessors();
        }
    };

    static {
        Core.getJDKContext().register(RESOURCE);
    }

    /**
     * This method returns the same number as {@link Runtime#availableProcessors()}
     * but the invocation is cheaper as it does so by accessing a static field caching
     * the value. Therefore, it is not guaranteed to return the most up-to-date value.
     *
     * @return Number of CPUs.
     */
    public static int get() {
        return NCPU;
    }

    private CpuCount() {}
}
