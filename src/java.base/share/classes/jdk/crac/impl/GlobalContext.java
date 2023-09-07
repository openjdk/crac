package jdk.crac.impl;

import jdk.crac.Context;
import jdk.crac.Resource;
import sun.security.action.GetPropertyAction;

public class GlobalContext {
    private static final String GLOBAL_CONTEXT_IMPL_PROP = "jdk.crac.globalContext.impl";

    public static Context<Resource> createGlobalContextImpl() {
        String implName = GetPropertyAction.privilegedGetProperty(GLOBAL_CONTEXT_IMPL_PROP, "");
        return switch (implName) {
            case "BlockingOrderedContext" -> new BlockingOrderedContext<>();
            case "OrderedContext" -> new OrderedContext<>();
            default -> new OrderedContext<>(); // cannot report as System.out/err are not initialized yet
        };
    }
}
