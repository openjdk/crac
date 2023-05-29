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

        if (e instanceof InterruptedException) {
            throw new RuntimeException(e);
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
