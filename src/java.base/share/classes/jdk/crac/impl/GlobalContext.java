package jdk.crac.impl;

import jdk.crac.Context;
import jdk.crac.Resource;
import sun.security.action.GetPropertyAction;

public class GlobalContext {
    private static final String GLOBAL_CONTEXT_IMPL_PROP = "jdk.crac.globalContext.impl";
    private static final String GLOBAL_CONTEXT_IMPL_NAME =
        GetPropertyAction.privilegedGetProperty(GLOBAL_CONTEXT_IMPL_PROP, "OrderedContext");

    public static Context<Resource> createGlobalContextImpl() {
        return switch (GLOBAL_CONTEXT_IMPL_NAME) {
            case "BlockingOrderedContext" -> new BlockingOrderedContext<>();
            case "OrderedContext" -> new OrderedContext<>();
            default -> {
                System.err.println("Unknown " + GLOBAL_CONTEXT_IMPL_PROP + "=" + GLOBAL_CONTEXT_IMPL_NAME);
                yield new OrderedContext<>();
            }
        };
    }
}
