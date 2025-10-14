package jdk.internal.crac.mirror.impl;

import jdk.internal.crac.JDKResource;
import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;

public class GlobalContext {
    private static final String GLOBAL_CONTEXT_IMPL_PROP = "jdk.crac.globalContext.impl";

    public static class Score implements JDKResource {
        private static final Score instance = new Score();

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) {
            Context<?> ctx = jdk.internal.crac.mirror.Core.getGlobalContext();
            if (ctx instanceof OrderedContext<?> octx) {
                jdk.internal.crac.Score.setScore("jdk.crac.globalContext.size", octx.size());
            }
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) {
        }

        public static Score instance() {
            return instance;
        }
    };

    public static Context<Resource> createGlobalContextImpl() {
        String implName = System.getProperty(GLOBAL_CONTEXT_IMPL_PROP, "");
        return switch (implName) {
            case "BlockingOrderedContext" -> new BlockingOrderedContext<>();
            case "OrderedContext" -> new OrderedContext<>();
            default -> new OrderedContext<>(); // cannot report as System.out/err are not initialized yet
        };
    }
}
