/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package java.lang.ref;

import java.util.function.Consumer;
import jdk.internal.misc.VM;
import jdk.internal.vm.ContinuationSupport;

/**
 * Reference queues, to which registered reference objects are appended by the
 * garbage collector after the appropriate reachability changes are detected.
 *
 * <p>{@linkplain java.lang.ref##MemoryConsistency Memory consistency effects}:
 * The enqueueing of a reference to a queue (by the garbage collector, or by a
 * successful call to {@link Reference#enqueue})
 * <a href="{@docRoot}/java.base/java/util/concurrent/package-summary.html#MemoryVisibility"><i>happens-before</i></a>
 * the reference is removed from the queue by {@link ReferenceQueue#poll} or
 * {@link ReferenceQueue#remove}.
 *
 * @param <T> the type of the reference object
 *
 * @author   Mark Reinhold
 * @since    1.2
 */

public class ReferenceQueue<@jdk.internal.RequiresIdentity T> {
    private static class Null extends ReferenceQueue<Object> {
        @Override
        boolean enqueue(Reference<?> r) {
            return false;
        }
    }

    static final ReferenceQueue<Object> NULL_QUEUE = new Null();
    static final ReferenceQueue<Object> ENQUEUED = new Null();

    private volatile Reference<? extends T> head;
    private long queueLength = 0;

    private static class Lock { };
    private final Lock lock = new Lock();

    /**
     * Constructs a new reference-object queue.
     */
    public ReferenceQueue() {
    }

    private boolean enqueue0(Reference<? extends T> r) { // must hold lock
        // Check that since getting the lock this reference hasn't already been
        // enqueued (and even then removed)
        ReferenceQueue<?> queue = r.queue;
        if ((queue == NULL_QUEUE) || (queue == ENQUEUED)) {
            return false;
        }
        assert queue == this;
        // Self-loop end, so if a FinalReference it remains inactive.
        r.next = (head == null) ? r : head;
        head = r;
        queueLength++;
        // Update r.queue *after* adding to list, to avoid race
        // with concurrent enqueued checks and fast-path poll().
        // Volatiles ensure ordering.
        r.queue = ENQUEUED;
        if (r instanceof FinalReference) {
            VM.addFinalRefCount(1);
        }
        lock.notifyAll();
        return true;
    }

    private Reference<? extends T> poll0() { // must hold lock
        Reference<? extends T> r = head;
        if (r != null) {
            r.queue = NULL_QUEUE;
            // Update r.queue *before* removing from list, to avoid
            // race with concurrent enqueued checks and fast-path
            // poll().  Volatiles ensure ordering.
            @SuppressWarnings("unchecked")
            Reference<? extends T> rn = r.next;
            // Handle self-looped next as end of list designator.
            head = (rn == r) ? null : rn;
            // Self-loop next rather than setting to null, so if a
            // FinalReference it remains inactive.
            r.next = r;
            queueLength--;
            if (r instanceof FinalReference) {
                VM.addFinalRefCount(-1);
            }
            return r;
        }
        return null;
    }

    private Reference<? extends T> remove0(long timeout) throws InterruptedException { // must hold lock
        Reference<? extends T> r = poll0();
        if (r != null) return r;
        long start = System.nanoTime();
        for (;;) {
            lock.wait(timeout);
            r = poll0();
            if (r != null) return r;
            long end = System.nanoTime();
            timeout -= (end - start) / 1000_000;
            if (timeout <= 0) return null;
            start = end;
        }
    }

    private Reference<? extends T> remove0() throws InterruptedException { // must hold lock
        for (;;) {
            var r = poll0();
            if (r != null) return r;
            lock.wait();
        }
    }

    boolean enqueue(Reference<? extends T> r) { /* Called only by Reference class */
        synchronized (lock) {
            return enqueue0(r);
        }
    }

    /**
     * Polls this queue to see if a reference object is available.  If one is
     * available without further delay then it is removed from the queue and
     * returned.  Otherwise this method immediately returns {@code null}.
     *
     * @return  A reference object, if one was immediately available,
     *          otherwise {@code null}
     * @see java.lang.ref.Reference#enqueue()
     */
    public Reference<? extends T> poll() {
        if (head == null)
            return null;

        // Prevent a virtual thread from being preempted as this could potentially
        // deadlock with a carrier that is polling the same reference queue.
        ContinuationSupport.pinIfSupported();
        try {
            synchronized (lock) {
                return poll0();
            }
        } finally {
            ContinuationSupport.unpinIfSupported();
        }
    }

    Reference<? extends T> poll(long timeout) throws InterruptedException {
        synchronized (lock) {
            Reference<? extends T> r = poll0();
            if (r != null) return r;
            // any wake (including spurious) ends the wait
            //noinspection ResultOfMethodCallIgnored
            lock.wait(timeout);
            return poll0();
        }
    }

    void wakeup() {
        synchronized (lock) {
            lock.notifyAll();
        }
    }

    /**
     * Removes the next reference object in this queue, blocking until either
     * one becomes available or the given timeout period expires.
     *
     * <p> This method does not offer real-time guarantees: It schedules the
     * timeout as if by invoking the {@link Object#wait(long)} method.
     *
     * @param  timeout  If positive, block for up to {@code timeout}
     *                  milliseconds while waiting for a reference to be
     *                  added to this queue.  If zero, block indefinitely.
     *
     * @return  A reference object, if one was available within the specified
     *          timeout period, otherwise {@code null}
     *
     * @throws  IllegalArgumentException
     *          If the value of the timeout argument is negative
     *
     * @throws  InterruptedException
     *          If the timeout wait is interrupted
     *
     * @see java.lang.ref.Reference#enqueue()
     */
    public Reference<? extends T> remove(long timeout) throws InterruptedException {
        if (timeout < 0)
            throw new IllegalArgumentException("Negative timeout value");
        if (timeout == 0)
            return remove();

        synchronized (lock) {
            return remove0(timeout);
        }
    }

    /**
     * Removes the next reference object in this queue, blocking until one
     * becomes available.
     *
     * @return A reference object, blocking until one becomes available
     * @throws  InterruptedException  If the wait is interrupted
     * @see java.lang.ref.Reference#enqueue()
     */
    public Reference<? extends T> remove() throws InterruptedException {
        synchronized (lock) {
            return remove0();
        }
    }

    /**
     * Iterate queue and invoke given action with each Reference.
     * Suitable for diagnostic purposes.
     * WARNING: any use of this method should make sure to not
     * retain the referents of iterated references (in case of
     * FinalReference(s)) so that their life is not prolonged more
     * than necessary.
     */
    void forEach(Consumer<? super Reference<? extends T>> action) {
        for (Reference<? extends T> r = head; r != null;) {
            action.accept(r);
            @SuppressWarnings("unchecked")
            Reference<? extends T> rn = r.next;
            if (rn == r) {
                if (r.queue == ENQUEUED) {
                    // still enqueued -> we reached end of chain
                    r = null;
                } else {
                    // already dequeued: r.queue == NULL_QUEUE; ->
                    // restart from head when overtaken by queue poller(s)
                    r = head;
                }
            } else {
                // next in chain
                r = rn;
            }
        }
    }
}
