package jdk.crac.impl;

import jdk.crac.CheckpointException;
import jdk.crac.RestoreException;

import java.io.PrintStream;
import java.io.PrintWriter;
import java.util.Objects;
import java.util.stream.Stream;

public final class ExceptionPrinter {
    private static final String MSG_FORMAT = "%s: Failed with %s inner exceptions%n";
    private static final String CAUSE_FORMAT = "Cause %d/%d: ";

    public static void print(Exception exception, PrintStream s) {
        assertExceptionType(exception);
        Throwable[] suppressed = exception.getSuppressed();
        s.printf(MSG_FORMAT, exception.getClass().getName(), suppressed.length);
        for (int i = 0; i < suppressed.length; ++i) {
            s.printf(CAUSE_FORMAT, i + 1, suppressed.length);
            suppressed[i].printStackTrace(s);
        }
    }

    public static void print(Exception exception, PrintWriter w) {
        assertExceptionType(exception);
        Throwable[] suppressed = exception.getSuppressed();
        w.printf(MSG_FORMAT, exception.getClass().getName(), suppressed.length);
        for (int i = 0; i < suppressed.length; ++i) {
            w.printf(CAUSE_FORMAT, i + 1, suppressed.length);
            suppressed[i].printStackTrace(w);
        }
    }

    private static void assertExceptionType(Exception exception) {
        Objects.requireNonNull(exception);
        if (Stream.of(CheckpointException.class, RestoreException.class,
                javax.crac.CheckpointException.class, javax.crac.RestoreException.class)
                .noneMatch(cls -> cls.isInstance(exception))) {
            throw new IllegalArgumentException("This printer is meant only for C/R exceptions", exception);
        }
    }
}
