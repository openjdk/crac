package jdk.crac.impl;

import jdk.crac.Context;
import jdk.crac.Resource;
import sun.security.action.GetPropertyAction;

public class GlobalContext {
    private static final String IMPL_PROP = "jdk.crac.globalContext.impl";
    private static final String IMPL_NAME =
        GetPropertyAction.privilegedGetProperty(IMPL_PROP, "");

    public static Context<Resource> createGlobalContextImpl() {
        return switch (IMPL_NAME) {
            case "BlockingOrderedContext" -> new BlockingOrderedContext<>();
            case "OrderedContext" -> new OrderedContext<>();
            default -> new OrderedContext<>(); // cannot report as System.out/err are not initialized yet
        };
    }
}
