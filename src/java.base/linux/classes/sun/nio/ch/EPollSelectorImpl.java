/*
 * Copyright (c) 2005, 2024, Oracle and/or its affiliates. All rights reserved.
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

import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;
import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKResource;
import jdk.internal.crac.mirror.impl.CheckpointOpenResourceException;
import jdk.internal.crac.mirror.impl.CheckpointOpenSocketException;

import java.io.FileDescriptor;
import java.io.IOException;
import java.io.Serial;
import java.nio.channels.SelectableChannel;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.spi.SelectorProvider;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.stream.Collectors;

import static sun.nio.ch.EPoll.EPOLLIN;
import static sun.nio.ch.EPoll.EPOLL_CTL_ADD;
import static sun.nio.ch.EPoll.EPOLL_CTL_DEL;
import static sun.nio.ch.EPoll.EPOLL_CTL_MOD;


/**
 * Linux epoll based Selector implementation
 *
 * @crac The file descriptor(s) used internally by this class are automatically
 * closed before checkpointing the process and opened after the restore.
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
    private Set<SelectableChannel> currentChannels;

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
        Core.Priority.EPOLLSELECTOR.getContext().register(this);
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
                currentChannels = fdToKey.values().stream().map(SelectionKey::channel).collect(Collectors.toSet());
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

        if (Thread.currentThread().isVirtual()) {
            numEntries = (timedPoll)
                    ? timedPoll(TimeUnit.MILLISECONDS.toNanos(to))
                    : untimedPoll(blocking);
        } else {
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
                        to -= (int) TimeUnit.NANOSECONDS.toMillis(adjust);
                        if (to <= 0) {
                            // timeout expired so no retry
                            numEntries = 0;
                        }
                    }
                } while (numEntries == IOStatus.INTERRUPTED);
            } finally {
                end(blocking);
            }
        }
        assert IOStatus.check(numEntries);

        processDeregisterQueue();
        return processEvents(numEntries, action);
    }

    /**
     * If blocking, parks the current virtual thread until a file descriptor is polled
     * or the thread is interrupted.
     */
    private int untimedPoll(boolean block) throws IOException {
        int numEntries = EPoll.wait(epfd, pollArrayAddress, NUM_EPOLLEVENTS, 0);
        if (block) {
            while (numEntries == 0 && !Thread.currentThread().isInterrupted()) {
                Poller.pollSelector(epfd, 0);
                numEntries = EPoll.wait(epfd, pollArrayAddress, NUM_EPOLLEVENTS, 0);
            }
        }
        return numEntries;
    }

    /**
     * Parks the current virtual thread until a file descriptor is polled, or the thread
     * is interrupted, for up to the specified waiting time.
     */
    private int timedPoll(long nanos) throws IOException {
        long startNanos = System.nanoTime();
        int numEntries = EPoll.wait(epfd, pollArrayAddress, NUM_EPOLLEVENTS, 0);
        while (numEntries == 0 && !Thread.currentThread().isInterrupted()) {
            long remainingNanos = nanos - (System.nanoTime() - startNanos);
            if (remainingNanos <= 0) {
                // timeout
                break;
            }
            Poller.pollSelector(epfd, remainingNanos);
            numEntries = EPoll.wait(epfd, pollArrayAddress, NUM_EPOLLEVENTS, 0);
        }
        return numEntries;
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
                var ex = new BusySelectorException("Selector " + this + " has registered keys from channels: " + currentChannels, null);
                ex.epollFds.add(claimFd(this.epfd, "EPoll FD "));
                ex.epollFds.add(claimFd(this.eventfd.efd(), "EPoll Event FD "));
                currentChannels = null;
                throw ex;
            }
        }
    }

    private FileDescriptor claimFd(int fdval, String type) {
        FileDescriptor fd = IOUtil.newFD(fdval);
        Core.getClaimedFDs().claimFd(fd, this,
                () -> new CheckpointOpenSocketException(type + fdval + " left open in " + this + " with registered keys.", null));
        return fd;
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

    private static class BusySelectorException extends CheckpointOpenResourceException {
        @Serial
        private static final long serialVersionUID = 5615481252774343456L;
        // We need to keep the FileDescriptors around until the checkpoint completes
        // as ClaimedFDs use WeakHashMap. Transient because exception is serializable
        // and FileDescriptor is not.
        transient List<FileDescriptor> epollFds = new ArrayList<>();

        public BusySelectorException(String details, Throwable cause) {
            super(details, cause);
        }
    }
}
