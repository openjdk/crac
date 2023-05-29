package jdk.crac.impl;

import jdk.internal.crac.LoggerContainer;

/**
 * The state of the Context
 */
public class BlockingState {
    private boolean blocked = false;

    // This method has particularly verbose name to stick out in thread dumps
    // when the registration leads to a deadlock.
    private void waitWhileCheckpointIsInProgress() throws InterruptedException {
        LoggerContainer.debug(Thread.currentThread().getName() + " waiting in " + this);
        try {
            wait();
        } catch (InterruptedException ie) {
            // FIXME there should be no interrupt re-set once we're going to throw InterruptedException
            Thread.currentThread().interrupt();
            throw ie;
        }
    }

    public synchronized void block() {
        blocked = true;
    }

    public synchronized void unblock() {
        blocked = false;
        notifyAll();
    }

    public synchronized void test() throws RuntimeException {
        try {
            while (blocked) {
                waitWhileCheckpointIsInProgress();
            }
        } catch (InterruptedException ie) {
            ie.printStackTrace();
            throw new RuntimeException(ie);
        }
    }
}
