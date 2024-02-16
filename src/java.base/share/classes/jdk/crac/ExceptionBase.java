package jdk.crac;

import jdk.crac.impl.ExceptionPrinter;

import java.io.PrintStream;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Arrays;

/**
 * Common base for {@link CheckpointException} and {@link RestoreException}
 */
public abstract class ExceptionBase extends Exception {
    private static final long serialVersionUID = -8131840538114148870L;
    private static final Throwable[] EMPTY_THROWABLE_ARRAY = new Throwable[0];
    private ArrayList<Throwable> nested;

    ExceptionBase() {
        super(null, null, false, false);
    }

    ExceptionBase(Throwable[] nested) {
        this();
        if (nested != null && nested.length > 0) {
            this.nested = new ArrayList<>(Arrays.asList(nested));
        }
    }

    /**
     * Add exception to the list of nested exceptions.
     * @param throwable Added exception
     */
    public void addNestedException(Throwable throwable) {
        if (this.nested == null) {
            this.nested = new ArrayList<>();
        }
        this.nested.add(throwable);
    }

    /**
     * Returns an array containing all the exceptions that this exception holds as nested exceptions.
     * @return an array containing all nested exceptions.
     */
    public Throwable[] getNestedExceptions() {
        if (nested == null) {
            return EMPTY_THROWABLE_ARRAY;
        }
        return nested.toArray(EMPTY_THROWABLE_ARRAY);
    }

    @Override
    public void printStackTrace(PrintStream s) {
        ExceptionPrinter.print(this, s);
    }

    @Override
    public void printStackTrace(PrintWriter w) {
        ExceptionPrinter.print(this, w);
    }
}