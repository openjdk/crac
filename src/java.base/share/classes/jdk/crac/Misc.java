package jdk.crac;

import jdk.internal.access.SharedSecrets;

import java.lang.ref.ReferenceQueue;

/**
 * Additional utilities.
 */
public final class Misc {

    private Misc() {
    }

    /**
     * Blocks calling thread until there are no references in the queue and
     * the specified number of threads are blocked without a reference to
     * process.
     * <p>
     * Note that {@code timeout} specifies the timeout for threads to block,
     * while the total time of this function to complete may be much larger
     * that the specified timeout.
     *
     * @param queue the queue to wait
     * @param nThreads number of threads to wait
     * @param timeout milliseconds to wait for threads to block, if positive,
     *                wait indefinitely, if zero,
     *                and, otherwise, just check the condition
     * @throws InterruptedException If the wait is interrupted
     * @return true if condition was true during the timeout period,
     *         otherwise false
     */
    public static boolean waitForQueueProcessed(ReferenceQueue<?> queue,
                                                int nThreads,
                                                long timeout)
        throws InterruptedException
    {
        return SharedSecrets.getJavaLangRefAccess().
            waitForQueueProcessed(queue, nThreads, timeout);
    }
}
