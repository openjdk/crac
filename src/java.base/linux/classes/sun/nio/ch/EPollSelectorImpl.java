/*
 * Copyright (c) 2005, 2021, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2020, 2021, Azul Systems, Inc. All rights reserved.
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

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;
import jdk.internal.crac.JDKResource;
import jdk.internal.crac.JDKResource.Priority;

import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.channels.ClosedSelectorException;
import java.nio.channels.IllegalSelectorException;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.spi.SelectorProvider;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

import static sun.nio.ch.EPoll.EPOLLIN;
import static sun.nio.ch.EPoll.EPOLL_CTL_ADD;
import static sun.nio.ch.EPoll.EPOLL_CTL_DEL;
import static sun.nio.ch.EPoll.EPOLL_CTL_MOD;


/**
 * Linux epoll based Selector implementation
 */

class EPollSelectorImpl extends SelectorImpl implements JDKResource {

    // maximum number of events to poll in one call to epoll_wait
    private static final int NUM_EPOLLEVENTS = Math.min(IOUtil.fdLimit(), 1024);

    private static final JavaIOFileDescriptorAccess fdAccess
            = SharedSecrets.getJavaIOFileDescriptorAccess();

    private enum CheckpointRestoreState {
        NORMAL_OPERATION,
        CHECKPOINT_TRANSITION,
        CHECKPOINTED,
        CHECKPOINT_ERROR,
        RESTORE_TRANSITION,
    }

    private static class MoveToCheckpointThread extends Thread {
        private Selector selector;

        MoveToCheckpointThread(Selector selector) {
            this.selector = selector;
        }

        @Override
        public void run() {
            try {
                selector.select(1);
            } catch (IOException e) {
            }
        }
    }

    // epoll file descriptor
    private int epfd;

    // address of poll array when polling with epoll_wait
    private long pollArrayAddress;

    // eventfd object used for interrupt
    private EventFD eventfd;

    // maps file descriptor to selection key, synchronize on selector
    private final Map<Integer, SelectionKeyImpl> fdToKey = new HashMap<>();

    // pending new registrations/updates, queued by setEventOps
    private final Object updateLock = new Object();
    private final Deque<SelectionKeyImpl> updateKeys = new ArrayDeque<>();

    // interrupt triggering and clearing
    private final Object interruptLock = new Object();
    private boolean interruptTriggered;

    private volatile CheckpointRestoreState checkpointState = CheckpointRestoreState.NORMAL_OPERATION;;

    private void initFDs() throws IOException {
        epfd = EPoll.create();

        try {
            this.eventfd = new EventFD();
            FileDescriptor fd = IOUtil.newFD(eventfd.efd());
            // This FileDescriptor is a one-time use, the actual FD will be closed from EventFD
            fdAccess.markClosed(fd);
            IOUtil.configureBlocking(fd, false);
        } catch (IOException ioe) {
            EPoll.freePollArray(pollArrayAddress);
            FileDispatcherImpl.closeIntFD(epfd);
            throw ioe;
        }

        // register the eventfd object for wakeups
        EPoll.ctl(epfd, EPOLL_CTL_ADD, eventfd.efd(), EPOLLIN);
    }

    EPollSelectorImpl(SelectorProvider sp) throws IOException {
        super(sp);
        pollArrayAddress = EPoll.allocatePollArray(NUM_EPOLLEVENTS);
        initFDs();
        // trigger FileDispatcherImpl initialization
        new FileDispatcherImpl();
        jdk.internal.crac.Core.getJDKContext().register(this);
    }

    private void ensureOpen() {
        if (!isOpen())
            throw new ClosedSelectorException();
    }

    private boolean processCheckpointRestore() throws IOException {
        assert Thread.holdsLock(this);

        if (checkpointState != CheckpointRestoreState.CHECKPOINT_TRANSITION) {
            return false;
        }

        synchronized (interruptLock) {

            CheckpointRestoreState thisState;
            if (fdToKey.size() == 0) {
                eventfd.close();
                eventfd = null;
                FileDispatcherImpl.closeIntFD(epfd);
                thisState = CheckpointRestoreState.CHECKPOINTED;
            } else {
                thisState = CheckpointRestoreState.CHECKPOINT_ERROR;
            }

            checkpointState = thisState;
            interruptLock.notifyAll();
            while (checkpointState == thisState) {
                try {
                    interruptLock.wait();
                } catch (InterruptedException e) {
                }
            }

            assert checkpointState == CheckpointRestoreState.RESTORE_TRANSITION;
            if (thisState == CheckpointRestoreState.CHECKPOINTED) {
                initFDs();
            }
            checkpointState = CheckpointRestoreState.NORMAL_OPERATION;
            interruptLock.notifyAll();

            if (interruptTriggered) {
                try {
                    eventfd.set();
                } catch (IOException ioe) {
                    throw new InternalError(ioe);
                }
            }
        }

        return true;
    }

    @Override
    protected int doSelect(Consumer<SelectionKey> action, long timeout)
        throws IOException
    {
        assert Thread.holdsLock(this);

        // epoll_wait timeout is int
        int to = (int) Math.min(timeout, Integer.MAX_VALUE);
        boolean blocking = (to != 0);
        boolean timedPoll = (to > 0);

        int numEntries;
        processUpdateQueue();
        processDeregisterQueue();
        try {
            begin(blocking);

            do {
                long startTime = timedPoll ? System.nanoTime() : 0;
                do {
                    numEntries = EPoll.wait(epfd, pollArrayAddress, NUM_EPOLLEVENTS, to);
                } while (processCheckpointRestore());
                if (numEntries == IOStatus.INTERRUPTED && timedPoll) {
                    // timed poll interrupted so need to adjust timeout
                    long adjust = System.nanoTime() - startTime;
                    to -= TimeUnit.MILLISECONDS.convert(adjust, TimeUnit.NANOSECONDS);
                    if (to <= 0) {
                        // timeout expired so no retry
                        numEntries = 0;
                    }
                }
            } while (numEntries == IOStatus.INTERRUPTED);
            assert IOStatus.check(numEntries);

        } finally {
            end(blocking);
        }
        processDeregisterQueue();
        return processEvents(numEntries, action);
    }

    /**
     * Process changes to the interest ops.
     */
    private void processUpdateQueue() {
        assert Thread.holdsLock(this);

        synchronized (updateLock) {
            SelectionKeyImpl ski;
            while ((ski = updateKeys.pollFirst()) != null) {
                if (ski.isValid()) {
                    int fd = ski.getFDVal();
                    // add to fdToKey if needed
                    SelectionKeyImpl previous = fdToKey.putIfAbsent(fd, ski);
                    assert (previous == null) || (previous == ski);

                    int newEvents = ski.translateInterestOps();
                    int registeredEvents = ski.registeredEvents();
                    if (newEvents != registeredEvents) {
                        if (newEvents == 0) {
                            // remove from epoll
                            EPoll.ctl(epfd, EPOLL_CTL_DEL, fd, 0);
                        } else {
                            if (registeredEvents == 0) {
                                // add to epoll
                                EPoll.ctl(epfd, EPOLL_CTL_ADD, fd, newEvents);
                            } else {
                                // modify events
                                EPoll.ctl(epfd, EPOLL_CTL_MOD, fd, newEvents);
                            }
                        }
                        ski.registeredEvents(newEvents);
                    }
                }
            }
        }
    }

    /**
     * Process the polled events.
     * If the interrupt fd has been selected, drain it and clear the interrupt.
     */
    private int processEvents(int numEntries, Consumer<SelectionKey> action)
        throws IOException
    {
        assert Thread.holdsLock(this);

        boolean interrupted = false;
        int numKeysUpdated = 0;
        for (int i=0; i<numEntries; i++) {
            long event = EPoll.getEvent(pollArrayAddress, i);
            int fd = EPoll.getDescriptor(event);
            if (fd == eventfd.efd()) {
                interrupted = true;
            } else {
                SelectionKeyImpl ski = fdToKey.get(fd);
                if (ski != null) {
                    int rOps = EPoll.getEvents(event);
                    numKeysUpdated += processReadyEvents(rOps, ski, action);
                }
            }
        }

        if (interrupted && !(Thread.currentThread() instanceof MoveToCheckpointThread)) {
            clearInterrupt();
        }

        return numKeysUpdated;
    }

    @Override
    protected void implClose() throws IOException {
        assert Thread.holdsLock(this);

        // prevent further wakeup
        synchronized (interruptLock) {
            interruptTriggered = true;
        }

        FileDispatcherImpl.closeIntFD(epfd);
        EPoll.freePollArray(pollArrayAddress);

        eventfd.close();
    }

    @Override
    protected void implDereg(SelectionKeyImpl ski) throws IOException {
        assert !ski.isValid();
        assert Thread.holdsLock(this);

        int fd = ski.getFDVal();
        if (fdToKey.remove(fd) != null) {
            if (ski.registeredEvents() != 0) {
                EPoll.ctl(epfd, EPOLL_CTL_DEL, fd, 0);
                ski.registeredEvents(0);
            }
        } else {
            assert ski.registeredEvents() == 0;
        }
    }

    @Override
    public void setEventOps(SelectionKeyImpl ski) {
        ensureOpen();
        synchronized (updateLock) {
            updateKeys.addLast(ski);
        }
    }

    @Override
    public Selector wakeup() {
        synchronized (interruptLock) {
            if (!interruptTriggered) {
                try {
                    eventfd.set();
                } catch (IOException ioe) {
                    throw new InternalError(ioe);
                }
                interruptTriggered = true;
            }
        }
        return this;
    }

    private void clearInterrupt() throws IOException {
        synchronized (interruptLock) {
            eventfd.reset();
            interruptTriggered = false;
        }
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        if (!isOpen()) {
            return;
        }

        synchronized (interruptLock) {
            checkpointState = CheckpointRestoreState.CHECKPOINT_TRANSITION;
            eventfd.set();
            int tries = 5;
            while (checkpointState == CheckpointRestoreState.CHECKPOINT_TRANSITION && 0 < tries--) {
                try {
                    interruptLock.wait(5);
                } catch (InterruptedException e) {
                }
            }
            if (checkpointState == CheckpointRestoreState.CHECKPOINT_TRANSITION) {
                Thread thr = new MoveToCheckpointThread(this);
                thr.setDaemon(true);
                thr.start();
            }
            while (checkpointState == CheckpointRestoreState.CHECKPOINT_TRANSITION) {
                try {
                    interruptLock.wait();
                } catch (InterruptedException e) {
                }
            }
            if (checkpointState == CheckpointRestoreState.CHECKPOINT_ERROR) {
                throw new IllegalSelectorException();
            }
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        if (!isOpen()) {
            return;
        }

        synchronized (interruptLock) {
            checkpointState = CheckpointRestoreState.RESTORE_TRANSITION;
            interruptLock.notifyAll();
            while (checkpointState == CheckpointRestoreState.RESTORE_TRANSITION) {
                try {
                    interruptLock.wait();
                } catch (InterruptedException e) {
                }
            }
        }
    }

    @Override
    public Priority getPriority() {
        return Priority.EPOLLSELECTOR;
    }
}
