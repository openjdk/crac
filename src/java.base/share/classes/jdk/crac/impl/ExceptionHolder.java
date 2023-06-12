package jdk.crac.impl;

import java.util.function.Supplier;

public class ExceptionHolder<E extends Exception> {
    E exception = null;
    final Supplier<E> constructor;

    public ExceptionHolder(Supplier<E> constructor) {
        this.constructor = constructor;
    }

    public E get() {
        if (exception == null) {
            exception = constructor.get();
        }
        return exception;
    }

    public void throwIfAny() throws E {
        if (exception != null) {
            throw exception;
        }
    }

    public void handle(Exception e) throws RuntimeException {
        if (e == null) {
            return;
        }

        E exception = get();
        if (exception.getClass() == e.getClass()) {
            for (Throwable t : e.getSuppressed()) {
                exception.addSuppressed(t);
            }
        } else {
            if (e instanceof InterruptedException) {
                // FIXME interrupt re-set should be up to the Context implementation, as
                // some implementations may prefer to continue beforeCheckpoint/afterRestore
                // notification, rather than exiting as soon as possible.
                Thread.currentThread().interrupt();
            }
            exception.addSuppressed(e);
        }
    }
}
