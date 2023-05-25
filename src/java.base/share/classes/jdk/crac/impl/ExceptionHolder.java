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

    public E getIfAny() {
        return exception;
    }

    public void handle(Exception e) {
        if (e == null) {
            return;
        }

        E exception = get();
        if (exception.getClass() == e.getClass()) {
            if (e.getMessage() != null) {
                exception.addSuppressed(e); // FIXME avoid message / preserve it via a distinct Exception
            }
            for (Throwable t : e.getSuppressed()) {
                exception.addSuppressed(t);
            }
        } else {
            // FIXME there is no reason to report interruption additionally
            //  along the exception
            if (e instanceof InterruptedException) {
                Thread.currentThread().interrupt();
            }
            exception.addSuppressed(e);
        }
    }

    @FunctionalInterface
    interface Block {
        void run() throws Exception;
    }

    public void runWithHandler(Block block) {
        try {
            block.run();
        } catch (Exception e) {
            handle(e);
        }
    }
}
