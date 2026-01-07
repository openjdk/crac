/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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
import jdk.internal.crac.mirror.impl.CheckpointOpenResourceException;
import jdk.internal.crac.mirror.impl.CheckpointOpenSocketException;

import java.io.FileDescriptor;
import java.io.IOException;
import java.io.Serial;
import java.nio.channels.ClosedSelectorException;
import java.nio.channels.SelectableChannel;
import java.nio.channels.Selector;
import java.nio.channels.spi.SelectorProvider;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Set;

public abstract class SelectorCRaCSupport extends SelectorImpl implements JDKResource {

    // interrupt triggering and clearing
    protected final Object interruptLock = new Object();
    protected volatile boolean interruptTriggered;

    private volatile CheckpointRestoreState checkpointState = CheckpointRestoreState.NORMAL_OPERATION;
    private Set<SelectableChannel> currentChannels;

    @SuppressWarnings("this-escape")
    protected SelectorCRaCSupport(SelectorProvider sp) {
        super(sp);
        // trigger FileDispatcherImpl initialization
        new FileDispatcherImpl();
        Core.Priority.SELECTOR.getContext().register(this);
    }

    protected abstract void initFileDescriptors(boolean restore) throws IOException;

    protected boolean processCheckpointRestore() throws IOException {
        assert Thread.holdsLock(this);

        if (checkpointState != CheckpointRestoreState.CHECKPOINT_TRANSITION) {
            return false;
        }
        // If the channel using this selector was closed, some keys might be cancelled
        // and we shall remove them.
        processDeregisterQueue();

        synchronized (interruptLock) {

            CheckpointRestoreState thisState;
            currentChannels = getRegisteredChannels();
            if (currentChannels.isEmpty()) {
                closeFileDescriptors();
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
                initFileDescriptors(true);
            }
            checkpointState = CheckpointRestoreState.NORMAL_OPERATION;
            interruptLock.notifyAll();

            if (interruptTriggered) {
                try {
                    wakeupInternal();
                } catch (IOException ioe) {
                    throw new InternalError(ioe);
                }
            }
        }

        return true;
    }

    protected abstract void closeFileDescriptors() throws IOException;

    protected abstract Set<SelectableChannel> getRegisteredChannels();

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        if (!isOpen()) {
            return;
        }

        synchronized (interruptLock) {
            checkpointState = CheckpointRestoreState.CHECKPOINT_TRANSITION;
            wakeupInternal();
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
                ex.fds.addAll(claimFileDescriptors());
                currentChannels = null;
                throw ex;
            }
        }
    }

    protected boolean shouldClearInterrupt() {
        return !(Thread.currentThread() instanceof MoveToCheckpointThread);
    }

    protected abstract void wakeupInternal() throws IOException;

    protected FileDescriptor claimFileDescriptor(int fdval, String type) {
        FileDescriptor fd = IOUtil.newFD(fdval);
        Core.getClaimedFDs().claimFd(fd, this,
                () -> new CheckpointOpenSocketException(type + fdval + " left open in " + this + " with registered keys.", null));
        return fd;
    }

    // In case of C/R exceptions, claims file descriptors that would be otherwise automatically handled
    protected abstract Collection<FileDescriptor> claimFileDescriptors();

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
            } catch (IOException | ClosedSelectorException e) {
            }
        }
    }

    static class BusySelectorException extends CheckpointOpenResourceException {
        @Serial
        private static final long serialVersionUID = 5615481252774343456L;
        // We need to keep the FileDescriptors around until the checkpoint completes
        // as ClaimedFDs use WeakHashMap. Transient because exception is serializable
        // and FileDescriptor is not.
        transient List<FileDescriptor> fds = new ArrayList<>();

        public BusySelectorException(String details, Throwable cause) {
            super(details, cause);
        }
    }
}
