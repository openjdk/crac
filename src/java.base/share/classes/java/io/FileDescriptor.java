/*
 * Copyright (c) 2003, 2025, Oracle and/or its affiliates. All rights reserved.
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

package java.io;

import java.util.*;
import java.util.function.Supplier;
import java.util.regex.Pattern;

import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.impl.CheckpointOpenResourceException;
import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;
import jdk.internal.crac.*;
import jdk.internal.misc.Blocker;
import jdk.internal.ref.PhantomCleanable;

/**
 * Instances of the file descriptor class serve as an opaque handle
 * to the underlying machine-specific structure representing an open
 * file, an open socket, or another source or sink of bytes.
 * The main practical use for a file descriptor is to create a
 * {@link FileInputStream} or {@link FileOutputStream} to contain it.
 * <p>
 * Applications should not create their own file descriptors.
 *
 * @author  Pavani Diwanji
 * @since   1.0
 */
public final class FileDescriptor {

    private int fd;

    private long handle;

    private Closeable parent;
    private List<Closeable> otherParents;
    private boolean closed;

    class Resource extends JDKFdResource {
        private boolean closedByNIO;

        @SuppressWarnings("fallthrough")
        @Override
        public void beforeCheckpoint(Context<? extends jdk.internal.crac.mirror.Resource> context) throws Exception {
            if (!closedByNIO && valid()) {
                ClaimedFDs claimedFDs = Core.getClaimedFDs();
                FileDescriptor self = FileDescriptor.this;
                String nativeDescription = nativeDescription0();

                OpenResourcePolicies.Policy policy = findPolicy(nativeDescription);
                String action = "error";
                Supplier<Exception> supplier = null;
                // Normally the claiming should be overridden by FileInputStream/FileOutputStream
                // but in case these are collected we handle FDs 0..2 here as well.
                if (policy != null) {
                    action = policy.action;
                } else if (self == in || self == out || self == err) {
                    action = "ignore";
                }
                supplier = switch (action.toLowerCase()) {
                    case "error":
                        yield () -> new CheckpointOpenResourceException(
                            FileDescriptor.class.getSimpleName() + " " + fd + ": " + nativeDescription,
                            getStackTraceHolder());
                    case "close":
                        close();
                    case "ignore":
                        warnOpenResource(policy, "File descriptor " + fd);
                        yield NO_EXCEPTION;
                    default: throw new IllegalArgumentException("Unknown policy action for file descriptor " + fd + ": " + action);
                };
                claimedFDs.claimFd(self, self, supplier);
            }
        }

        private OpenResourcePolicies.Policy findPolicy(String nativeDescription) {
            return OpenResourcePolicies.find(false, "filedescriptor", params -> {
                String value = params.get("value");
                if (value != null) {
                    try {
                        int expected = Integer.parseInt(value);
                        if (expected != fd) {
                            return false;
                        }
                    } catch (NumberFormatException e) {
                        throw new IllegalArgumentException("Cannot parse file descriptor value '" + value + "'");
                    }
                }
                String regex = params.get("regex");
                if (regex != null) {
                    return Pattern.compile(regex).matcher(nativeDescription).find();
                }
                return true;
            });
        }

        @Override
        public String toString() {
            return getClass().getName() + "(FD " + fd + ")";
        }
    }

    Resource resource = new Resource();

    /**
     * true, if file is opened for appending.
     */
    private boolean append;

    static {
        initIDs();
    }

    // Set up JavaIOFileDescriptorAccess in SharedSecrets
    static {
        SharedSecrets.setJavaIOFileDescriptorAccess(
                new JavaIOFileDescriptorAccess() {
                    public void set(FileDescriptor fdo, int fd) {
                        fdo.set(fd);
                    }

                    public int get(FileDescriptor fdo) {
                        return fdo.fd;
                    }

                    public void setAppend(FileDescriptor fdo, boolean append) {
                        fdo.append = append;
                    }

                    public boolean getAppend(FileDescriptor fdo) {
                        return fdo.append;
                    }

                    public void close(FileDescriptor fdo) throws IOException {
                        fdo.close();
                    }

                    @Override
                    public void markClosed(FileDescriptor fdo) {
                        fdo.resource.closedByNIO = true;
                    }

                    /* Register for a normal FileCleanable fd/handle cleanup. */
                    public void registerCleanup(FileDescriptor fdo) {
                        FileCleanable.register(fdo);
                    }

                    /* Register a custom PhantomCleanup. */
                    public void registerCleanup(FileDescriptor fdo,
                                                PhantomCleanable<FileDescriptor> cleanup) {
                        fdo.registerCleanup(cleanup);
                    }

                    public void unregisterCleanup(FileDescriptor fdo) {
                        fdo.unregisterCleanup();
                    }

                    public void setHandle(FileDescriptor fdo, long handle) {
                        fdo.setHandle(handle);
                    }

                    public long getHandle(FileDescriptor fdo) {
                        return fdo.handle;
                    }
                }
        );
    }

    /**
     * Cleanup in case FileDescriptor is not explicitly closed.
     */
    private PhantomCleanable<FileDescriptor> cleanup;

    /**
     * Constructs an (invalid) FileDescriptor object.
     * The fd or handle is set later.
     */
    public FileDescriptor() {
        fd = -1;
        handle = -1;
    }

    /**
     * Used for standard input, output, and error only.
     * For Windows the corresponding handle is initialized.
     * For Unix the append mode is cached.
     * @param fd the raw fd number (0, 1, 2)
     */
    private FileDescriptor(int fd) {
        this.fd = fd;
        this.handle = getHandle(fd);
        this.append = getAppend(fd);
    }

    /**
     * A handle to the standard input stream. Usually, this file
     * descriptor is not used directly, but rather via the input stream
     * known as {@code System.in}.
     *
     * @see     java.lang.System#in
     */
    public static final FileDescriptor in = new FileDescriptor(0);

    /**
     * A handle to the standard output stream. Usually, this file
     * descriptor is not used directly, but rather via the output stream
     * known as {@code System.out}.
     * @see     java.lang.System#out
     */
    public static final FileDescriptor out = new FileDescriptor(1);

    /**
     * A handle to the standard error stream. Usually, this file
     * descriptor is not used directly, but rather via the output stream
     * known as {@code System.err}.
     *
     * @see     java.lang.System#err
     */
    public static final FileDescriptor err = new FileDescriptor(2);

    /**
     * Tests if this file descriptor object is valid.
     *
     * @return  {@code true} if the file descriptor object represents a
     *          valid, open file, socket, or other active I/O connection;
     *          {@code false} otherwise.
     */
    public boolean valid() {
        return (handle != -1) || (fd != -1);
    }

    /**
     * Force all system buffers to synchronize with the underlying
     * device.  This method returns after all modified data and
     * attributes of this FileDescriptor have been written to the
     * relevant device(s).  In particular, if this FileDescriptor
     * refers to a physical storage medium, such as a file in a file
     * system, sync will not return until all in-memory modified copies
     * of buffers associated with this FileDescriptor have been
     * written to the physical medium.
     *
     * sync is meant to be used by code that requires physical
     * storage (such as a file) to be in a known state  For
     * example, a class that provided a simple transaction facility
     * might use sync to ensure that all changes to a file caused
     * by a given transaction were recorded on a storage medium.
     *
     * sync only affects buffers downstream of this FileDescriptor.  If
     * any in-memory buffering is being done by the application (for
     * example, by a BufferedOutputStream object), those buffers must
     * be flushed into the FileDescriptor (for example, by invoking
     * OutputStream.flush) before that data will be affected by sync.
     *
     * @throws    SyncFailedException
     *        Thrown when the buffers cannot be flushed,
     *        or because the system cannot guarantee that all the
     *        buffers have been synchronized with physical media.
     * @since     1.1
     */
    public void sync() throws SyncFailedException {
        boolean attempted = Blocker.begin();
        try {
            sync0();
        } finally {
            Blocker.end(attempted);
        }
    }

    /* fsync/equivalent this file descriptor */
    private native void sync0() throws SyncFailedException;

    /* This routine initializes JNI field offsets for the class */
    private static native void initIDs();

    /*
     * On Windows return the handle for the standard streams.
     */
    private static native long getHandle(int d);

    /**
     * Returns true, if the file was opened for appending.
     */
    private static native boolean getAppend(int fd);

    /**
     * Set the fd.
     * Used on Unix and for sockets on Windows and Unix.
     * If setting to -1, clear the cleaner.
     * The {@link #registerCleanup} method should be called for new fds.
     * @param fd the raw fd or -1 to indicate closed
     */
    synchronized void set(int fd) {
        if (fd == -1 && cleanup != null) {
            cleanup.clear();
            cleanup = null;
        }
        this.fd = fd;
    }

    /**
     * Set the handle.
     * Used on Windows for regular files.
     * If setting to -1, clear the cleaner.
     * The {@link #registerCleanup} method should be called for new handles.
     * @param handle the handle or -1 to indicate closed
     */
    void setHandle(long handle) {
        if (handle == -1 && cleanup != null) {
            cleanup.clear();
            cleanup = null;
        }
        this.handle = handle;
    }

    /**
     * Register a cleanup for the current handle.
     * Used directly in java.io and indirectly via fdAccess.
     * The cleanup should be registered after the handle is set in the FileDescriptor.
     * @param cleanable a PhantomCleanable to register
     */
    synchronized void registerCleanup(PhantomCleanable<FileDescriptor> cleanable) {
        Objects.requireNonNull(cleanable, "cleanable");
        if (cleanup != null) {
            cleanup.clear();
        }
        cleanup = cleanable;
    }

    /**
     * Unregister a cleanup for the current raw fd or handle.
     * Used directly in java.io and indirectly via fdAccess.
     * Normally {@link #close()} should be used except in cases where
     * it is certain the caller will close the raw fd and the cleanup
     * must not close the raw fd.  {@link #unregisterCleanup()} must be
     * called before the raw fd is closed to prevent a race that makes
     * it possible for the fd to be reallocated to another use and later
     * the cleanup might be invoked.
     */
    synchronized void unregisterCleanup() {
        if (cleanup != null) {
            cleanup.clear();
        }
        cleanup = null;
    }

    /**
     * Close the raw file descriptor or handle, if it has not already been closed.
     * The native code sets the fd and handle to -1.
     * Clear the cleaner so the close does not happen twice.
     * Package private to allow it to be used in java.io.
     * @throws IOException if close fails
     */
    synchronized void close() throws IOException {
        unregisterCleanup();
        close0();
    }

    private native String nativeDescription0();

    /*
     * Close the raw file descriptor or handle, if it has not already been closed
     * and set the fd and handle to -1.
     */
    private native void close0() throws IOException;

    /*
     * Package private methods to track referents.
     * If multiple streams point to the same FileDescriptor, we cycle
     * through the list of all referents and call close()
     */

    /**
     * Attach a Closeable to this FD for tracking.
     * parent reference is added to otherParents when
     * needed to make closeAll simpler.
     */
    synchronized void attach(Closeable c) {
        if (parent == null) {
            // first caller gets to do this
            parent = c;
        } else if (otherParents == null) {
            otherParents = new ArrayList<>();
            otherParents.add(parent);
            otherParents.add(c);
        } else {
            otherParents.add(c);
        }
    }

    /**
     * Cycle through all Closeables sharing this FD and call
     * close() on each one.
     *
     * The caller closeable gets to call close0().
     */
    synchronized void closeAll(Closeable releaser) throws IOException {
        if (!closed) {
            closed = true;
            IOException ioe = null;
            try (releaser) {
                if (otherParents != null) {
                    for (Closeable referent : otherParents) {
                        try {
                            referent.close();
                        } catch(IOException x) {
                            if (ioe == null) {
                                ioe = x;
                            } else {
                                ioe.addSuppressed(x);
                            }
                        }
                    }
                }
            } catch(IOException ex) {
                /*
                 * If releaser close() throws IOException
                 * add other exceptions as suppressed.
                 */
                if (ioe != null)
                    ex.addSuppressed(ioe);
                ioe = ex;
            } finally {
                if (ioe != null)
                    throw ioe;
            }
        }
    }
}
