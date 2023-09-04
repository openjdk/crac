package jdk.crac.impl;

import jdk.crac.Context;
import jdk.crac.Resource;
import sun.security.action.GetPropertyAction;

public class GlobalContext {
    public static Context<Resource> createGlobalContextImpl() {
        String impl_prop = "jdk.crac.globalContext.impl";
        String impl_name = GetPropertyAction.privilegedGetProperty(impl_prop, "");
        return switch (impl_name) {
            case "BlockingOrderedContext" -> new BlockingOrderedContext<>();
            case "OrderedContext" -> new OrderedContext<>();
            default -> new OrderedContext<>(); // cannot report as System.out/err are not initialized yet
        };
    }
}
