package jdk.crac.impl;

import jdk.crac.ExceptionBase;

import java.io.PrintStream;
import java.io.PrintWriter;
import java.util.Objects;

public final class ExceptionPrinter {
    private static final String MSG_FORMAT = "%s: Failed with %s nested exceptions%n";
    private static final String CAUSE_FORMAT = "Cause %d/%d: ";

    public static void print(Exception exception, PrintStream s) {
        Throwable[] nested = getNestedExceptions(exception);
        s.printf(MSG_FORMAT, exception.getClass().getName(), nested.length);
        for (int i = 0; i < nested.length; ++i) {
            s.printf(CAUSE_FORMAT, i + 1, nested.length);
            nested[i].printStackTrace(s);
        }
    }

    public static void print(Exception exception, PrintWriter w) {
        Throwable[] nested = getNestedExceptions(exception);
        w.printf(MSG_FORMAT, exception.getClass().getName(), nested.length);
        for (int i = 0; i < nested.length; ++i) {
            w.printf(CAUSE_FORMAT, i + 1, nested.length);
            nested[i].printStackTrace(w);
        }
    }

    private static Throwable[] getNestedExceptions(Exception exception) {
        Objects.requireNonNull(exception);
        if (exception instanceof ExceptionBase ex) {
            return ex.getNestedExceptions();
        } else if (exception instanceof javax.crac.ExceptionBase ex) {
            return ex.getNestedExceptions();
        } else {
            throw new IllegalArgumentException("This printer is meant only for C/R exceptions", exception);
        }
    }
}
