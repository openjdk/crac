package sun.awt;

import java.awt.Window;
import java.awt.Cursor;

import sun.awt.X11.XRootWindow;
import sun.awt.X11.XWindow;
import sun.awt.X11.XBaseWindow;
import sun.awt.X11.XGlobalCursorManager;
import sun.awt.X11.XWM;
import sun.awt.X11.XAtom;
import sun.awt.X11.XToolkit;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.JDKResource;


public class X11AWTJDKResource implements JDKResource {

    @Override
    public Priority getPriority() {
        return Priority.X11AWT;
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        // AWT
        Window.resource.beforeCheckpoint(context);
        Cursor.resource.beforeCheckpoint(context);

        // X11
        XRootWindow.resource.beforeCheckpoint(context);
        XWindow.resource.beforeCheckpoint(context);
        XBaseWindow.resource.beforeCheckpoint(context);
        XGlobalCursorManager.resource.beforeCheckpoint(context);
        XWM.resource.beforeCheckpoint(context);
        XAtom.resource.beforeCheckpoint(context);
        XToolkit.resource.beforeCheckpoint(context);
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        // X11
        XToolkit.resource.afterRestore(context);
        XAtom.resource.afterRestore(context);
        XWM.resource.afterRestore(context);
        XGlobalCursorManager.resource.afterRestore(context);
        XBaseWindow.resource.afterRestore(context);
        XWindow.resource.afterRestore(context);
        XRootWindow.resource.afterRestore(context);

        // AWT
        Cursor.resource.afterRestore(context);
        Window.resource.afterRestore(context);
    }
}
