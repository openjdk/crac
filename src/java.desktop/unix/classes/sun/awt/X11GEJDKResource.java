package sun.awt;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.JDKResource;

import java.awt.GraphicsEnvironment;


public class X11GEJDKResource implements JDKResource {

    @Override
    public Priority getPriority() {
        return Priority.X11GE;
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        // GE
        GraphicsEnvironment.resource.beforeCheckpoint(context);

        // X11
        X11GraphicsEnvironment.resource.beforeCheckpoint(context);
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        // X11
        X11GraphicsEnvironment.resource.afterRestore(context);

        // GE
        GraphicsEnvironment.resource.afterRestore(context);
    }
}
