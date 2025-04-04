/*
 * Copyright (c) 2008, 2022, Oracle and/or its affiliates. All rights reserved.
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

package sun.nio.ch;

import jdk.internal.crac.Core;
import jdk.internal.crac.JDKResource;
import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;

import java.nio.channels.spi.AsynchronousChannelProvider;
import java.io.IOException;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Phaser;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicInteger;

import static sun.nio.ch.EPoll.EPOLLIN;
import static sun.nio.ch.EPoll.EPOLLONESHOT;
import static sun.nio.ch.EPoll.EPOLL_CTL_ADD;
import static sun.nio.ch.EPoll.EPOLL_CTL_MOD;


/**
 * AsynchronousChannelGroup implementation based on the Linux epoll facility.
 */

final class EPollPort
    extends Port
{
    // maximum number of events to poll at a time
    private static final int MAX_EPOLL_EVENTS = 512;

    // errors
    private static final int ENOENT     = 2;

    // epoll file descriptor
    private int epfd;

    // address of the poll array passed to epoll_wait
    private final long address;

    // true if epoll closed
    private boolean closed;

    // socket pair used for wakeup
    private final int sp[];

    // number of wakeups pending
    private final AtomicInteger wakeupCount = new AtomicInteger();

    // encapsulates an event for a channel
    static class Event {
        final PollableChannel channel;
        final int events;

        Event(PollableChannel channel, int events) {
            this.channel = channel;
            this.events = events;
        }

        PollableChannel channel()   { return channel; }
        int events()                { return events; }
    }

    // queue of events for cases that a polling thread dequeues more than one
    // event
    private final ArrayBlockingQueue<Event> queue;
    private final Event NEED_TO_POLL = new Event(null, 0);
    private final Event EXECUTE_TASK_OR_SHUTDOWN = new Event(null, 0);
    private final Event CHECKPOINT = new Event(null, 0);

    private final CRaCResource resource = new CRaCResource();

    private class CRaCResource implements JDKResource {
        // The field is volatile since it's accessed in isCheckpoint() which might be called
        // after a wakeup, without any guarantees for memory barriers.
        private volatile Phaser phaser;
        // Not volatile as it's accessed only after receiving CHECKPOINT event through the
        // queue which establishes a happens-after relation.
        private AtomicInteger counter;
        private IOException restoreException;

        @Override
        public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
            int threads = threadCount();
            if (threads == 0) {
                throw new IllegalStateException();
            }
            phaser = new Phaser(threadCount() + 1);
            counter = new AtomicInteger(threads);
            for (int i = 0; i < threads; ++i) {
                // cannot use executeOnHandlerTask as taskQueue is null in a non-fixed threadpool
                queue.offer(CHECKPOINT);
            }
            // we call wakeup only once since there's only one thread actually polling
            wakeup();
            // synchronization 1: wait until other threads enter processCheckpoint
            phaser.arriveAndAwaitAdvance();
        }

        private void processCheckpoint() {
            boolean isLast = counter.decrementAndGet() == 0;
            if (isLast) {
                // This code is closing epfd even if there are FDs registered in that; the existence
                // of these FDs will cause their own exceptions on checkpoint.
                // If these are ignored by FD policies it's up to the user to deal with missed registrations.
                try { FileDispatcherImpl.closeIntFD(epfd); } catch (IOException ioe) { }
                try { FileDispatcherImpl.closeIntFD(sp[0]); } catch (IOException ioe) { }
                try { FileDispatcherImpl.closeIntFD(sp[1]); } catch (IOException ioe) { }
            }
            // synchronization 1: threads entering processCheckpoint()
            phaser.arriveAndAwaitAdvance();
            // synchronization 2: block threads until afterRestore()
            phaser.arriveAndAwaitAdvance();
            if (isLast) {
                try {
                    epfd = EPoll.create();
                    long fds = IOUtil.makePipe(true);
                    sp[0] = (int) (fds >>> 32);
                    sp[1] = (int) fds;
                } catch (IOException e) {
                    restoreException = e;
                }
            }
            // synchronization 3: notify that FDs have been re-created
            phaser.arrive();
        }

        @Override
        public void afterRestore(Context<? extends Resource> context) throws Exception {
            // synchronization 2: unblock threads waiting until restore
            phaser.arriveAndAwaitAdvance();
            // synchronization 3: wait until all threads re-create the FDs
            phaser.arriveAndAwaitAdvance();
            counter = null;
            phaser = null;
            if (restoreException != null) {
                Exception e = restoreException;
                restoreException = null;
                throw e;
            }
        }

        public boolean isCheckpoint() {
            return phaser != null;
        }
    }

    EPollPort(AsynchronousChannelProvider provider, ThreadPool pool)
        throws IOException
    {
        super(provider, pool);

        this.epfd = EPoll.create();
        this.address = EPoll.allocatePollArray(MAX_EPOLL_EVENTS);

        // create socket pair for wakeup mechanism
        try {
            long fds = IOUtil.makePipe(true);
            this.sp = new int[]{(int) (fds >>> 32), (int) fds};
        } catch (IOException ioe) {
            EPoll.freePollArray(address);
            FileDispatcherImpl.closeIntFD(epfd);
            throw ioe;
        }

        // register one end with epoll
        EPoll.ctl(epfd, EPOLL_CTL_ADD, sp[0], EPOLLIN);

        // create the queue and offer the special event to ensure that the first
        // threads polls
        this.queue = new ArrayBlockingQueue<>(MAX_EPOLL_EVENTS);
        this.queue.offer(NEED_TO_POLL);

        Core.Priority.EPOLLSELECTOR.getContext().register(resource);
    }

    EPollPort start() {
        startThreads(new EventHandlerTask());
        return this;
    }

    /**
     * Release all resources
     */
    private void implClose() {
        synchronized (this) {
            if (closed)
                return;
            closed = true;
        }
        try { FileDispatcherImpl.closeIntFD(epfd); } catch (IOException ioe) { }
        try { FileDispatcherImpl.closeIntFD(sp[0]); } catch (IOException ioe) { }
        try { FileDispatcherImpl.closeIntFD(sp[1]); } catch (IOException ioe) { }
        EPoll.freePollArray(address);
    }

    private void wakeup() {
        if (wakeupCount.incrementAndGet() == 1) {
            // write byte to socketpair to force wakeup
            try {
                IOUtil.write1(sp[1], (byte)0);
            } catch (IOException x) {
                throw new AssertionError(x);
            }
        }
    }

    @Override
    void executeOnHandlerTask(Runnable task) {
        synchronized (this) {
            if (closed)
                throw new RejectedExecutionException();
            offerTask(task);
            wakeup();
        }
    }

    @Override
    void shutdownHandlerTasks() {
        /*
         * If no tasks are running then just release resources; otherwise
         * write to the one end of the socketpair to wakeup any polling threads.
         */
        int nThreads = threadCount();
        if (nThreads == 0) {
            implClose();
        } else {
            // send wakeup to each thread
            while (nThreads-- > 0) {
                wakeup();
            }
        }
    }

    // invoke by clients to register a file descriptor
    @Override
    void startPoll(int fd, int events) {
        // update events (or add to epoll on first usage)
        int err = EPoll.ctl(epfd, EPOLL_CTL_MOD, fd, (events | EPOLLONESHOT));
        if (err == ENOENT)
            err = EPoll.ctl(epfd, EPOLL_CTL_ADD, fd, (events | EPOLLONESHOT));
        if (err != 0)
            throw new AssertionError();     // should not happen
    }

    /**
     * Task to process events from epoll and dispatch to the channel's
     * onEvent handler.
     *
     * Events are retrieved from epoll in batch and offered to a BlockingQueue
     * where they are consumed by handler threads. A special "NEED_TO_POLL"
     * event is used to signal one consumer to re-poll when all events have
     * been consumed.
     */
    private class EventHandlerTask implements Runnable {
        private Event poll() throws IOException {
            try {
                for (;;) {
                    int n;
                    do {
                        n = EPoll.wait(epfd, address, MAX_EPOLL_EVENTS, -1);
                    } while (n == IOStatus.INTERRUPTED);

                    /**
                     * 'n' events have been read. Here we map them to their
                     * corresponding channel in batch and queue n-1 so that
                     * they can be handled by other handler threads. The last
                     * event is handled by this thread (and so is not queued).
                     */
                    fdToChannelLock.readLock().lock();
                    try {
                        while (n-- > 0) {
                            long eventAddress = EPoll.getEvent(address, n);
                            int fd = EPoll.getDescriptor(eventAddress);

                            // wakeup
                            if (fd == sp[0]) {
                                if (wakeupCount.decrementAndGet() == 0) {
                                    // consume one wakeup byte, never more as this
                                    // would interfere with shutdown when there is
                                    // a wakeup byte queued to wake each thread
                                    int nread;
                                    do {
                                        nread = IOUtil.drain1(sp[0]);
                                    } while (nread == IOStatus.INTERRUPTED);
                                }

                                // queue special event if there are more events
                                // to handle.
                                if (n > 0) {
                                    queue.offer(EXECUTE_TASK_OR_SHUTDOWN);
                                    continue;
                                }
                                return EXECUTE_TASK_OR_SHUTDOWN;
                            }

                            PollableChannel channel = fdToChannel.get(fd);
                            if (channel != null) {
                                int events = EPoll.getEvents(eventAddress);
                                Event ev = new Event(channel, events);

                                // n-1 events are queued; This thread handles
                                // the last one except for the wakeup
                                if (n > 0) {
                                    queue.offer(ev);
                                } else {
                                    return ev;
                                }
                            }
                        }
                    } finally {
                        fdToChannelLock.readLock().unlock();
                    }
                }
            } finally {
                // to ensure that some thread will poll when all events have
                // been consumed
                queue.offer(NEED_TO_POLL);
            }
        }

        public void run() {
            Invoker.GroupAndInvokeCount myGroupAndInvokeCount =
                Invoker.getGroupAndInvokeCount();
            final boolean isPooledThread = (myGroupAndInvokeCount != null);
            boolean replaceMe = false;
            Event ev;
            try {
                for (;;) {
                    // reset invoke count
                    if (isPooledThread)
                        myGroupAndInvokeCount.resetInvokeCount();

                    try {
                        replaceMe = false;
                        ev = queue.take();

                        // no events and this thread has been "selected" to
                        // poll for more.
                        if (ev == NEED_TO_POLL) {
                            try {
                                ev = poll();
                            } catch (IOException x) {
                                x.printStackTrace();
                                return;
                            }
                        }
                    } catch (InterruptedException x) {
                        continue;
                    }

                    // handle wakeup to execute task or shutdown
                    if (ev == EXECUTE_TASK_OR_SHUTDOWN) {
                        if (resource.isCheckpoint()) {
                            // the wakeup was caused by checkpoint, but there might not be any taskQueue
                            // and no task. It is not a shutdown, though.
                            continue;
                        }
                        Runnable task = pollTask();
                        if (task == null) {
                            // shutdown request
                            return;
                        }
                        // run task (may throw error/exception)
                        replaceMe = true;
                        task.run();
                        continue;
                    } else if (ev == CHECKPOINT) {
                        resource.processCheckpoint();
                        continue;
                    }

                    // process event
                    try {
                        ev.channel().onEvent(ev.events(), isPooledThread);
                    } catch (Error | RuntimeException x) {
                        replaceMe = true;
                        throw x;
                    }
                }
            } finally {
                // last handler to exit when shutdown releases resources
                int remaining = threadExit(this, replaceMe);
                if (remaining == 0 && isShutdown()) {
                    implClose();
                }
            }
        }
    }
}
