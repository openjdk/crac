/*
 * Copyright (c) 1994, 2025, Oracle and/or its affiliates. All rights reserved.
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

import java.nio.channels.FileChannel;

import jdk.internal.crac.OpenResourcePolicies;
import jdk.internal.access.JavaIORandomAccessFileAccess;
import jdk.internal.access.SharedSecrets;
import jdk.internal.crac.JDKFileResource;
import jdk.internal.misc.Blocker;
import jdk.internal.util.ByteArray;
import jdk.internal.event.FileReadEvent;
import jdk.internal.event.FileWriteEvent;
import sun.nio.ch.FileChannelImpl;


/**
 * Instances of this class support both reading and writing to a
 * random access file. A random access file behaves like a large
 * array of bytes stored in the file system. There is a kind of cursor,
 * or index into the implied array, called the <em>file pointer</em>;
 * input operations read bytes starting at the file pointer and advance
 * the file pointer past the bytes read. If the random access file is
 * created in read/write mode, then output operations are also available;
 * output operations write bytes starting at the file pointer and advance
 * the file pointer past the bytes written. Output operations that write
 * past the current end of the implied array cause the array to be
 * extended. The file pointer can be read by the
 * {@code getFilePointer} method and set by the {@code seek}
 * method.
 * <p>
 * It is generally true of all the reading routines in this class that
 * if end-of-file is reached before the desired number of bytes has been
 * read, an {@code EOFException} (which is a kind of
 * {@code IOException}) is thrown. If any byte cannot be read for
 * any reason other than end-of-file, an {@code IOException} other
 * than {@code EOFException} is thrown. In particular, an
 * {@code IOException} may be thrown if the stream has been closed.
 *
 * @since   1.0
 */

public class RandomAccessFile implements DataOutput, DataInput, Closeable {

    private static final int O_RDONLY = 1;
    private static final int O_RDWR =   2;
    private static final int O_SYNC =   4;
    private static final int O_DSYNC =  8;
    private static final int O_TEMPORARY =  16;

    /**
     * Flag set by jdk.internal.event.JFRTracing to indicate if
     * file reads and writes should be traced by JFR.
     */
    private static boolean jfrTracing;

    private final FileDescriptor fd;

    private final boolean rw;
    private final boolean sync;  // O_SYNC or O_DSYNC
    private final int imode;

    /**
     * The path of the referenced file
     * (null if the stream is created with a file descriptor)
     */
    private final String path;

    private final Object closeLock = new Object();

    /**
     * A local buffer that allows reading and writing of
     * longer primitive parameters (e.g. long) to be performed
     * using bulk operations rather than on a per-byte basis.
     */
    private final byte[] buffer = new byte[Long.BYTES];

    private volatile FileChannel channel;
    private volatile boolean closed;

    private final JDKFileResource resource = new JDKFileResource() {
        private long offset;

        @Override
        protected FileDescriptor getFD() {
            return fd;
        }

        @Override
        protected String getPath() {
            return path;
        }

        @Override
        protected void closeBeforeCheckpoint(OpenResourcePolicies.Policy policy) throws IOException {
            // We cannot synchronize this without making every other method call synchronized
            offset = getFilePointer();
            close();
        }

        @Override
        protected void reopenAfterRestore(OpenResourcePolicies.Policy policy) throws IOException {
            synchronized (closeLock) {
                open(path, imode);
                seek(offset);
                RandomAccessFile.this.closed = false;
                FileCleanable.register(fd);
            }
        }
    };

    /**
     * Creates a random access file stream to read from, and optionally
     * to write to, a file with the specified pathname. If the file exists
     * it is opened; if it does not exist and write mode is specified, a
     * new file is created.
     * {@linkplain java.nio.file##links Symbolic links}
     * are automatically redirected to the <i>target</i> of the link.
     * A new {@link FileDescriptor} object is created to represent the
     * connection to the file.
     *
     * <p> The {@code mode} argument specifies the access mode with which the
     * file is to be opened.  The permitted values and their meanings are as
     * specified for the <a
     * href="#mode">{@code RandomAccessFile(File,String)}</a> constructor.
     *
     * @param      pathname   the system-dependent pathname string
     * @param      mode       the access <a href="#mode">mode</a>
     * @throws     IllegalArgumentException  if the mode argument is not equal
     *             to one of {@code "r"}, {@code "rw"}, {@code "rws"}, or
     *             {@code "rwd"}
     * @throws     FileNotFoundException
     *             if the mode is {@code "r"} but the given pathname string does not
     *             denote an existing regular file, or if the mode begins with
     *             {@code "rw"} but the given pathname string does not denote an
     *             existing, writable regular file and a new regular file of
     *             that pathname cannot be created, or if some other error
     *             occurs while opening or creating the file
     */
    public RandomAccessFile(String pathname, String mode)
        throws FileNotFoundException
    {
        this(pathname != null ? new File(pathname) : null, mode);
    }

    /**
     * Creates a random access file stream to read from, and optionally
     * to write to, the file specified by the {@link File} argument. If
     * the file exists it is opened; if it does not exist and write mode
     * is specified, a new file is created.
     * {@linkplain java.nio.file##links Symbolic links}
     * are automatically redirected to the <i>target</i> of the link.
     * A new {@link FileDescriptor} object is created to represent the
     * connection to the file.
     *
     * <p>The <a id="mode">{@code mode}</a> argument specifies the access mode
     * in which the file is to be opened.  The permitted values and their
     * meanings are:
     *
     * <table class="striped">
     * <caption style="display:none">Access mode permitted values and meanings</caption>
     * <thead>
     * <tr><th scope="col" style="text-align:left">Value</th><th scope="col" style="text-align:left">Meaning</th></tr>
     * </thead>
     * <tbody>
     * <tr><th scope="row" style="vertical-align:top">{@code "r"}</th>
     *     <td> Open for reading only. Invoking any of the {@code write}
     *     methods of the resulting object will cause an
     *     {@link java.io.IOException} to be thrown.</td></tr>
     * <tr><th scope="row" style="vertical-align:top">{@code "rw"}</th>
     *     <td> Open for reading and writing.  If the file does not already
     *     exist then an attempt will be made to create it.</td></tr>
     * <tr><th scope="row" style="vertical-align:top">{@code "rws"}</th>
     *     <td> Open for reading and writing, as with {@code "rw"}, and also
     *     require that every update to the file's content or metadata be
     *     written synchronously to the underlying storage device.</td></tr>
     * <tr><th scope="row" style="vertical-align:top">{@code "rwd"}</th>
     *     <td> Open for reading and writing, as with {@code "rw"}, and also
     *     require that every update to the file's content be written
     *     synchronously to the underlying storage device.</td></tr>
     * </tbody>
     * </table>
     *
     * The {@code "rws"} and {@code "rwd"} modes work much like the {@link
     * java.nio.channels.FileChannel#force(boolean) force(boolean)} method of
     * the {@link java.nio.channels.FileChannel} class, passing arguments of
     * {@code true} and {@code false}, respectively, except that they always
     * apply to every I/O operation and are therefore often more efficient.  If
     * the file resides on a local storage device then when an invocation of a
     * method of this class returns it is guaranteed that all changes made to
     * the file by that invocation will have been written to that device.  This
     * is useful for ensuring that critical information is not lost in the
     * event of a system crash.  If the file does not reside on a local device
     * then no such guarantee is made.
     *
     * <p>The {@code "rwd"} mode can be used to reduce the number of I/O
     * operations performed.  Using {@code "rwd"} only requires updates to the
     * file's content to be written to storage; using {@code "rws"} requires
     * updates to both the file's content and its metadata to be written, which
     * generally requires at least one more low-level I/O operation.
     *
     * @param      file   the file object
     * @param      mode   the access mode, as described
     *                    <a href="#mode">above</a>
     * @throws     IllegalArgumentException  if the mode argument is not equal
     *             to one of {@code "r"}, {@code "rw"}, {@code "rws"}, or
     *             {@code "rwd"}
     * @throws     FileNotFoundException
     *             if the mode is {@code "r"} but the given file object does
     *             not denote an existing regular file, or if the mode begins
     *             with {@code "rw"} but the given file object does not denote
     *             an existing, writable regular file and a new regular file of
     *             that pathname cannot be created, or if some other error
     *             occurs while opening or creating the file
     * @see        java.nio.channels.FileChannel#force(boolean)
     */
    @SuppressWarnings("this-escape")
    public RandomAccessFile(File file, String mode)
        throws FileNotFoundException
    {
        this(file, mode, false);
    }

    private RandomAccessFile(File file, String mode, boolean openAndDelete)
        throws FileNotFoundException
    {
        String name = (file != null ? file.getPath() : null);
        int imode = -1;

        boolean rw = false;
        boolean sync = false;
        if (mode.equals("r"))
            imode = O_RDONLY;
        else if (mode.startsWith("rw")) {
            imode = O_RDWR;
            rw = true;
            if (mode.length() > 2) {
                if (mode.equals("rws")) {
                    imode |= O_SYNC;
                    sync = true;
                } else if (mode.equals("rwd")) {
                    imode |= O_DSYNC;
                    sync = true;
                } else
                    imode = -1;
            }
        }
        this.rw = rw;
        this.sync = sync;

        if (openAndDelete)
            imode |= O_TEMPORARY;
        if (imode < 0)
            throw new IllegalArgumentException("Illegal mode \"" + mode
                                               + "\" must be one of "
                                               + "\"r\", \"rw\", \"rws\","
                                               + " or \"rwd\"");
        if (name == null) {
            throw new NullPointerException();
        }
        if (file.isInvalid()) {
            throw new FileNotFoundException("Invalid file path");
        }
        fd = new FileDescriptor();
        fd.attach(this);
        path = name;
        this.imode = imode;
        open(name, imode);
        FileCleanable.register(fd);   // open sets the fd, register the cleanup
    }

    /**
     * Returns the opaque file descriptor object associated with this
     * stream.
     *
     * @return     the file descriptor object associated with this stream.
     * @throws     IOException  if an I/O error occurs.
     * @see        java.io.FileDescriptor
     */
    public final FileDescriptor getFD() throws IOException {
        return fd;
    }

    /**
     * Returns the unique {@link java.nio.channels.FileChannel FileChannel}
     * object associated with this file.
     *
     * <p> The {@link java.nio.channels.FileChannel#position()
     * position} of the returned channel will always be equal to
     * this object's file-pointer offset as returned by the {@link
     * #getFilePointer getFilePointer} method.  Changing this object's
     * file-pointer offset, whether explicitly or by reading or writing bytes,
     * will change the position of the channel, and vice versa.  Changing the
     * file's length via this object will change the length seen via the file
     * channel, and vice versa.
     *
     * @return  the file channel associated with this file
     *
     * @since 1.4
     */
    public final FileChannel getChannel() {
        FileChannel fc = this.channel;
        if (fc == null) {
            synchronized (this) {
                fc = this.channel;
                if (fc == null) {
                    fc = FileChannelImpl.open(fd, path, true, rw, sync, false, this);
                    this.channel = fc;
                    if (closed) {
                        try {
                            fc.close();
                        } catch (IOException ioe) {
                            throw new InternalError(ioe); // should not happen
                        }
                    }
                }
            }
        }
        return fc;
    }

    /**
     * Opens a file and returns the file descriptor.  The file is
     * opened in read-write mode if the O_RDWR bit in {@code mode}
     * is true, else the file is opened as read-only.
     * If the {@code name} refers to a directory, an IOException
     * is thrown.
     *
     * @param name the name of the file
     * @param mode the mode flags, a combination of the O_ constants
     *             defined above
     */
    private native void open0(String name, int mode)
        throws FileNotFoundException;

    // wrap native call to allow instrumentation
    /**
     * Opens a file and returns the file descriptor.  The file is
     * opened in read-write mode if the O_RDWR bit in {@code mode}
     * is true, else the file is opened as read-only.
     * If the {@code name} refers to a directory, an IOException
     * is thrown.
     *
     * @param name the name of the file
     * @param mode the mode flags, a combination of the O_ constants
     *             defined above
     */
    private void open(String name, int mode) throws FileNotFoundException {
        open0(name, mode);
    }

    // 'Read' primitives

    /**
     * Reads a byte of data from this file. The byte is returned as an
     * integer in the range 0 to 255 ({@code 0x00-0x0ff}). This
     * method blocks if no input is yet available.
     * <p>
     * Although {@code RandomAccessFile} is not a subclass of
     * {@code InputStream}, this method behaves in exactly the same
     * way as the {@link InputStream#read()} method of
     * {@code InputStream}.
     *
     * @return     the next byte of data, or {@code -1} if the end of the
     *             file has been reached.
     * @throws     IOException  if an I/O error occurs. Not thrown if
     *                          end-of-file has been reached.
     */
    public int read() throws IOException {
        if (jfrTracing && FileReadEvent.enabled()) {
            return traceRead0();
        }
        return read0();
    }

    private native int read0() throws IOException;

    private int traceRead0() throws IOException {
        int result = 0;
        long bytesRead = 0;
        long start = FileReadEvent.timestamp();
        try {
            result = read0();
            if (result < 0) {
                bytesRead = -1;
            } else {
                bytesRead = 1;
            }
        } finally {
            FileReadEvent.offer(start, path, bytesRead);
        }
        return result;
    }

    /**
     * Reads a sub array as a sequence of bytes.
     * @param     b the buffer into which the data is read.
     * @param     off the start offset of the data.
     * @param     len the number of bytes to read.
     * @throws    IOException If an I/O error has occurred.
     */
    private int readBytes(byte[] b, int off, int len) throws IOException {
        if (jfrTracing && FileReadEvent.enabled()) {
            return traceReadBytes0(b, off, len);
        }
        return readBytes0(b, off, len);
    }

    private native int readBytes0(byte[] b, int off, int len) throws IOException;

    private int traceReadBytes0(byte b[], int off, int len) throws IOException {
        int bytesRead = 0;
        long start = FileReadEvent.timestamp();
        try {
            bytesRead = readBytes0(b, off, len);
        } finally {
            FileReadEvent.offer(start, path, bytesRead);
        }
        return bytesRead;
    }

    /**
     * Reads up to {@code len} bytes of data from this file into an
     * array of bytes. This method blocks until at least one byte of input
     * is available.
     * <p>
     * Although {@code RandomAccessFile} is not a subclass of
     * {@code InputStream}, this method behaves in exactly the
     * same way as the {@link InputStream#read(byte[], int, int)} method of
     * {@code InputStream}.
     *
     * @param      b     the buffer into which the data is read.
     * @param      off   the start offset in array {@code b}
     *                   at which the data is written.
     * @param      len   the maximum number of bytes read.
     * @return     the total number of bytes read into the buffer, or
     *             {@code -1} if there is no more data because the end of
     *             the file has been reached.
     * @throws     IOException If the first byte cannot be read for any reason
     *             other than end of file, or if the random access file has been closed,
     *             or if some other I/O error occurs.
     * @throws     NullPointerException If {@code b} is {@code null}.
     * @throws     IndexOutOfBoundsException If {@code off} is negative,
     *             {@code len} is negative, or {@code len} is greater than
     *             {@code b.length - off}
     */
    public int read(byte[] b, int off, int len) throws IOException {
        return readBytes(b, off, len);
    }

    /**
     * Reads up to {@code b.length} bytes of data from this file
     * into an array of bytes. This method blocks until at least one byte
     * of input is available.
     * <p>
     * Although {@code RandomAccessFile} is not a subclass of
     * {@code InputStream}, this method behaves in exactly the
     * same way as the {@link InputStream#read(byte[])} method of
     * {@code InputStream}.
     *
     * @param      b   the buffer into which the data is read.
     * @return     the total number of bytes read into the buffer, or
     *             {@code -1} if there is no more data because the end of
     *             this file has been reached.
     * @throws     IOException If the first byte cannot be read for any reason
     *             other than end of file, or if the random access file has been closed,
     *             or if some other I/O error occurs.
     * @throws     NullPointerException If {@code b} is {@code null}.
     */
    public int read(byte[] b) throws IOException {
        return readBytes(b, 0, b.length);
    }

    /**
     * Reads {@code b.length} bytes from this file into the byte
     * array, starting at the current file pointer. This method reads
     * repeatedly from the file until the requested number of bytes are
     * read. This method blocks until the requested number of bytes are
     * read, the end of the stream is detected, or an exception is thrown.
     *
     * @param   b   the buffer into which the data is read.
     * @throws  NullPointerException if {@code b} is {@code null}.
     * @throws  EOFException  if this file reaches the end before reading
     *              all the bytes.
     * @throws  IOException   if an I/O error occurs.
     */
    public final void readFully(byte[] b) throws IOException {
        readFully(b, 0, b.length);
    }

    /**
     * Reads exactly {@code len} bytes from this file into the byte
     * array, starting at the current file pointer. This method reads
     * repeatedly from the file until the requested number of bytes are
     * read. This method blocks until the requested number of bytes are
     * read, the end of the stream is detected, or an exception is thrown.
     *
     * @param   b     the buffer into which the data is read.
     * @param   off   the start offset into the data array {@code b}.
     * @param   len   the number of bytes to read.
     * @throws  NullPointerException if {@code b} is {@code null}.
     * @throws  IndexOutOfBoundsException if {@code off} is negative,
     *                {@code len} is negative, or {@code len} is greater than
     *                {@code b.length - off}.
     * @throws  EOFException  if this file reaches the end before reading
     *                all the bytes.
     * @throws  IOException   if an I/O error occurs.
     */
    public final void readFully(byte[] b, int off, int len) throws IOException {
        int n = 0;
        do {
            int count = this.read(b, off + n, len - n);
            if (count < 0)
                throw new EOFException();
            n += count;
        } while (n < len);
    }

    /**
     * Attempts to skip over {@code n} bytes of input discarding the
     * skipped bytes.
     * <p>
     *
     * This method may skip over some smaller number of bytes, possibly zero.
     * This may result from any of a number of conditions; reaching end of
     * file before {@code n} bytes have been skipped is only one
     * possibility. This method never throws an {@code EOFException}.
     * The actual number of bytes skipped is returned.  If {@code n}
     * is negative, no bytes are skipped.
     *
     * @param      n   the number of bytes to be skipped.
     * @return     the actual number of bytes skipped.
     * @throws     IOException  if an I/O error occurs.
     */
    public int skipBytes(int n) throws IOException {
        long pos;
        long len;
        long newpos;

        if (n <= 0) {
            return 0;
        }
        pos = getFilePointer();
        len = length();
        newpos = pos + n;
        if (newpos > len) {
            newpos = len;
        }
        seek(newpos);

        /* return the actual number of bytes skipped */
        return (int) (newpos - pos);
    }

    // 'Write' primitives

    /**
     * Writes the specified byte to this file. The write starts at
     * the current file pointer.
     *
     * @param      b   the {@code byte} to be written.
     * @throws     IOException  if an I/O error occurs.
     */
    public void write(int b) throws IOException {
        if (jfrTracing && FileWriteEvent.enabled()) {
            traceImplWrite(b);
            return;
        }
        implWrite(b);
    }

    private void implWrite(int b) throws IOException {
        boolean attempted = Blocker.begin(sync);
        try {
            write0(b);
        } finally {
            Blocker.end(attempted);
        }
    }

    private void traceImplWrite(int b) throws IOException {
        long bytesWritten = 0;
        long start = FileWriteEvent.timestamp();
        try {
            implWrite(b);
            bytesWritten = 1;
        } finally {
            FileWriteEvent.offer(start, path, bytesWritten);
        }
    }

    private native void write0(int b) throws IOException;

    /**
     * Writes a sub array as a sequence of bytes.
     *
     * @param     b the data to be written
     * @param     off the start offset in the data
     * @param     len the number of bytes that are written
     * @throws    IOException If an I/O error has occurred.
     */
    private void writeBytes(byte[] b, int off, int len) throws IOException {
        if (jfrTracing && FileWriteEvent.enabled()) {
            traceImplWriteBytes(b, off, len);
            return;
        }
        implWriteBytes(b, off, len);
    }

    private void implWriteBytes(byte[] b, int off, int len) throws IOException {
        boolean attempted = Blocker.begin(sync);
        try {
            writeBytes0(b, off, len);
        } finally {
            Blocker.end(attempted);
        }
    }

    private void traceImplWriteBytes(byte b[], int off, int len) throws IOException {
        long bytesWritten = 0;
        long start = FileWriteEvent.timestamp();
        try {
            implWriteBytes(b, off, len);
            bytesWritten = len;
        } finally {
            FileWriteEvent.offer(start, path, bytesWritten);
        }
    }

    private native void writeBytes0(byte[] b, int off, int len) throws IOException;

    /**
     * Writes {@code b.length} bytes from the specified byte array
     * to this file, starting at the current file pointer.
     *
     * @param      b   the data.
     * @throws     IOException  if an I/O error occurs.
     */
    public void write(byte[] b) throws IOException {
        writeBytes(b, 0, b.length);
    }

    /**
     * Writes {@code len} bytes from the specified byte array
     * starting at offset {@code off} to this file.
     *
     * @param      b     the data.
     * @param      off   the start offset in the data.
     * @param      len   the number of bytes to write.
     * @throws     IOException  if an I/O error occurs.
     * @throws     IndexOutOfBoundsException {@inheritDoc}
     */
    public void write(byte[] b, int off, int len) throws IOException {
        writeBytes(b, off, len);
    }

    // 'Random access' stuff

    /**
     * Returns the current offset in this file.
     *
     * @return     the offset from the beginning of the file, in bytes,
     *             at which the next read or write occurs.
     * @throws     IOException  if an I/O error occurs.
     */
    public native long getFilePointer() throws IOException;

    /**
     * Sets the file-pointer offset, measured from the beginning of this
     * file, at which the next read or write occurs.  The offset may be
     * set beyond the end of the file. Setting the offset beyond the end
     * of the file does not change the file length.  The file length will
     * change only by writing after the offset has been set beyond the end
     * of the file.
     *
     * @param      pos   the offset position, measured in bytes from the
     *                   beginning of the file, at which to set the file
     *                   pointer.
     * @throws     IOException  if {@code pos} is less than
     *                          {@code 0} or if an I/O error occurs.
     */
    public void seek(long pos) throws IOException {
        if (pos < 0) {
            throw new IOException("Negative seek offset");
        }
        seek0(pos);
    }

    private native void seek0(long pos) throws IOException;

    /**
     * Returns the length of this file.
     *
     * @return     the length of this file, measured in bytes.
     * @throws     IOException  if an I/O error occurs.
     */
    public long length() throws IOException {
        return length0();
    }

    private native long length0() throws IOException;

    /**
     * Sets the length of this file.
     *
     * <p> If the present length of the file as returned by the
     * {@linkplain #length length} method is greater than the desired length
     * of the file specified by the {@code newLength} argument, then the file
     * will be truncated.
     *
     * <p> If the present length of the file is smaller than the desired length,
     * then the file will be extended.  The contents of the extended portion of
     * the file are not defined.
     *
     * <p> If the present length of the file is equal to the desired length,
     * then the file and its length will be unchanged.
     *
     * <p> In all cases, after this method returns, the file offset as returned
     * by the {@linkplain #getFilePointer getFilePointer} method will equal the
     * minimum of the desired length and the file offset before this method was
     * called, even if the length is unchanged.  In other words, this method
     * constrains the file offset to the closed interval {@code [0,newLength]}.
     *
     * @param      newLength    The desired length of the file
     * @throws     IOException  If the argument is negative or
     *                          if some other I/O error occurs
     * @since      1.2
     */
    public void setLength(long newLength) throws IOException {
        setLength0(newLength);
    }

    private native void setLength0(long newLength) throws IOException;

    /**
     * Closes this random access file stream and releases any system
     * resources associated with the stream. A closed random access
     * file cannot perform input or output operations and cannot be
     * reopened.
     *
     * <p> If this file has an associated channel then the channel is closed
     * as well.
     *
     * @apiNote
     * If this stream has an associated channel then this method will close the
     * channel, which in turn will close this stream. Subclasses that override
     * this method should be prepared to handle possible reentrant invocation.
     *
     * @throws     IOException  if an I/O error occurs.
     */
    public void close() throws IOException {
        if (closed) {
            return;
        }
        synchronized (closeLock) {
            if (closed) {
                return;
            }
            closed = true;
        }

        FileChannel fc = channel;
        if (fc != null) {
            // possible race with getChannel(), benign since
            // FileChannel.close is final and idempotent
            fc.close();
        }

        fd.closeAll(new Closeable() {
            public void close() throws IOException {
               fd.close();
           }
        });
    }

    //
    //  Some "reading/writing Java data types" methods stolen from
    //  DataInputStream and DataOutputStream.
    //

    /**
     * Reads a {@code boolean} from this file. This method reads a
     * single byte from the file, starting at the current file pointer.
     * A value of {@code 0} represents
     * {@code false}. Any other value represents {@code true}.
     * This method blocks until the byte is read, the end of the stream
     * is detected, or an exception is thrown.
     *
     * @return     the {@code boolean} value read.
     * @throws     EOFException  if this file has reached the end.
     * @throws     IOException   if an I/O error occurs.
     */
    public final boolean readBoolean() throws IOException {
        return readUnsignedByte() != 0;
    }

    /**
     * Reads a signed eight-bit value from this file. This method reads a
     * byte from the file, starting from the current file pointer.
     * If the byte read is {@code b}, where
     * {@code 0 <= b <= 255},
     * then the result is:
     * {@snippet lang=java :
     *     (byte)(b)
     * }
     * <p>
     * This method blocks until the byte is read, the end of the stream
     * is detected, or an exception is thrown.
     *
     * @return     the next byte of this file as a signed eight-bit
     *             {@code byte}.
     * @throws     EOFException  if this file has reached the end.
     * @throws     IOException   if an I/O error occurs.
     */
    public final byte readByte() throws IOException {
        return (byte) readUnsignedByte();
    }

    /**
     * Reads an unsigned eight-bit number from this file. This method reads
     * a byte from this file, starting at the current file pointer,
     * and returns that byte.
     * <p>
     * This method blocks until the byte is read, the end of the stream
     * is detected, or an exception is thrown.
     *
     * @return     the next byte of this file, interpreted as an unsigned
     *             eight-bit number.
     * @throws     EOFException  if this file has reached the end.
     * @throws     IOException   if an I/O error occurs.
     */
    public final int readUnsignedByte() throws IOException {
        int ch = this.read();
        if (ch < 0)
            throw new EOFException();
        return ch;
    }

    /**
     * Reads a signed 16-bit number from this file. The method reads two
     * bytes from this file, starting at the current file pointer.
     * If the two bytes read, in order, are
     * {@code b1} and {@code b2}, where each of the two values is
     * between {@code 0} and {@code 255}, inclusive, then the
     * result is equal to:
     * {@snippet lang=java :
     *     (short)((b1 << 8) | b2)
     * }
     * <p>
     * This method blocks until the two bytes are read, the end of the
     * stream is detected, or an exception is thrown.
     *
     * @return     the next two bytes of this file, interpreted as a signed
     *             16-bit number.
     * @throws     EOFException  if this file reaches the end before reading
     *               two bytes.
     * @throws     IOException   if an I/O error occurs.
     */
    public final short readShort() throws IOException {
        return (short) readUnsignedShort();
    }

    /**
     * Reads an unsigned 16-bit number from this file. This method reads
     * two bytes from the file, starting at the current file pointer.
     * If the bytes read, in order, are
     * {@code b1} and {@code b2}, where
     * {@code 0 <= b1, b2 <= 255},
     * then the result is equal to:
     * {@snippet lang=java :
     *     (b1 << 8) | b2
     * }
     * <p>
     * This method blocks until the two bytes are read, the end of the
     * stream is detected, or an exception is thrown.
     *
     * @return     the next two bytes of this file, interpreted as an unsigned
     *             16-bit integer.
     * @throws     EOFException  if this file reaches the end before reading
     *               two bytes.
     * @throws     IOException   if an I/O error occurs.
     */
    public final int readUnsignedShort() throws IOException {
        readFully(buffer, 0, Short.BYTES);
        return  ((buffer[1] & 0xff)      ) +
                ((buffer[0] & 0xff) <<  8);
    }

    /**
     * Reads a character from this file. This method reads two
     * bytes from the file, starting at the current file pointer.
     * If the bytes read, in order, are
     * {@code b1} and {@code b2}, where
     * {@code 0 <= b1, b2 <= 255},
     * then the result is equal to:
     * {@snippet lang=java :
     *     (char)((b1 << 8) | b2)
     * }
     * <p>
     * This method blocks until the two bytes are read, the end of the
     * stream is detected, or an exception is thrown.
     *
     * @return     the next two bytes of this file, interpreted as a
     *                  {@code char}.
     * @throws     EOFException  if this file reaches the end before reading
     *               two bytes.
     * @throws     IOException   if an I/O error occurs.
     */
    public final char readChar() throws IOException {
        return (char) readUnsignedShort();
    }

    /**
     * Reads a signed 32-bit integer from this file. This method reads 4
     * bytes from the file, starting at the current file pointer.
     * If the bytes read, in order, are {@code b1},
     * {@code b2}, {@code b3}, and {@code b4}, where
     * {@code 0 <= b1, b2, b3, b4 <= 255},
     * then the result is equal to:
     * {@snippet lang=java :
     *     (b1 << 24) | (b2 << 16) + (b3 << 8) + b4
     * }
     * <p>
     * This method blocks until the four bytes are read, the end of the
     * stream is detected, or an exception is thrown.
     *
     * @return     the next four bytes of this file, interpreted as an
     *             {@code int}.
     * @throws     EOFException  if this file reaches the end before reading
     *               four bytes.
     * @throws     IOException   if an I/O error occurs.
     */
    public final int readInt() throws IOException {
        readFully(buffer, 0, Integer.BYTES);
        return ByteArray.getInt(buffer, 0);
    }

    /**
     * Reads a signed 64-bit integer from this file. This method reads eight
     * bytes from the file, starting at the current file pointer.
     * If the bytes read, in order, are
     * {@code b1}, {@code b2}, {@code b3},
     * {@code b4}, {@code b5}, {@code b6},
     * {@code b7}, and {@code b8,} where:
     * {@snippet :
     *     0 <= b1, b2, b3, b4, b5, b6, b7, b8 <= 255
     * }
     * <p>
     * then the result is equal to:
     * {@snippet lang=java :
     *     ((long)b1 << 56) + ((long)b2 << 48)
     *         + ((long)b3 << 40) + ((long)b4 << 32)
     *         + ((long)b5 << 24) + ((long)b6 << 16)
     *         + ((long)b7 << 8) + b8
     * }
     * <p>
     * This method blocks until the eight bytes are read, the end of the
     * stream is detected, or an exception is thrown.
     *
     * @return     the next eight bytes of this file, interpreted as a
     *             {@code long}.
     * @throws     EOFException  if this file reaches the end before reading
     *               eight bytes.
     * @throws     IOException   if an I/O error occurs.
     */
    public final long readLong() throws IOException {
        readFully(buffer, 0, Long.BYTES);
        return ByteArray.getLong(buffer, 0);
    }

    /**
     * Reads a {@code float} from this file. This method reads an
     * {@code int} value, starting at the current file pointer,
     * as if by the {@code readInt} method
     * and then converts that {@code int} to a {@code float}
     * using the {@code intBitsToFloat} method in class
     * {@code Float}.
     * <p>
     * This method blocks until the four bytes are read, the end of the
     * stream is detected, or an exception is thrown.
     *
     * @return     the next four bytes of this file, interpreted as a
     *             {@code float}.
     * @throws     EOFException  if this file reaches the end before reading
     *             four bytes.
     * @throws     IOException   if an I/O error occurs.
     * @see        java.io.RandomAccessFile#readInt()
     * @see        java.lang.Float#intBitsToFloat(int)
     */
    public final float readFloat() throws IOException {
        readFully(buffer, 0, Float.BYTES);
        return ByteArray.getFloat(buffer, 0);
    }

    /**
     * Reads a {@code double} from this file. This method reads a
     * {@code long} value, starting at the current file pointer,
     * as if by the {@code readLong} method
     * and then converts that {@code long} to a {@code double}
     * using the {@code longBitsToDouble} method in
     * class {@code Double}.
     * <p>
     * This method blocks until the eight bytes are read, the end of the
     * stream is detected, or an exception is thrown.
     *
     * @return     the next eight bytes of this file, interpreted as a
     *             {@code double}.
     * @throws     EOFException  if this file reaches the end before reading
     *             eight bytes.
     * @throws     IOException   if an I/O error occurs.
     * @see        java.io.RandomAccessFile#readLong()
     * @see        java.lang.Double#longBitsToDouble(long)
     */
    public final double readDouble() throws IOException {
        readFully(buffer, 0, Double.BYTES);
        return ByteArray.getDouble(buffer, 0);
    }

    /**
     * Reads the next line of text from this file.  This method successively
     * reads bytes from the file, starting at the current file pointer,
     * until it reaches a line terminator or the end
     * of the file.  Each byte is converted into a character by taking the
     * byte's value for the lower eight bits of the character and setting the
     * high eight bits of the character to zero.  This method does not,
     * therefore, support the full Unicode character set.
     *
     * <p> A line of text is terminated by a carriage-return character
     * ({@code '\u005Cr'}), a newline character ({@code '\u005Cn'}), a
     * carriage-return character immediately followed by a newline character,
     * or the end of the file.  Line-terminating characters are discarded and
     * are not included as part of the string returned.
     *
     * <p> This method blocks until a newline character is read, a carriage
     * return and the byte following it are read (to see if it is a newline),
     * the end of the file is reached, or an exception is thrown.
     *
     * @return     the next line of text from this file, or null if end
     *             of file is encountered before even one byte is read.
     * @throws     IOException  if an I/O error occurs.
     */

    public final String readLine() throws IOException {
        StringBuilder input = new StringBuilder();
        int c = -1;
        boolean eol = false;

        while (!eol) {
            switch (c = read()) {
                case -1, '\n' -> eol = true;
                case '\r'     -> {
                    eol = true;
                    long cur = getFilePointer();
                    if ((read()) != '\n') {
                        seek(cur);
                    }
                }
                default -> input.append((char) c);
            }
        }

        if ((c == -1) && (input.length() == 0)) {
            return null;
        }
        return input.toString();
    }

    /**
     * Reads in a string from this file. The string has been encoded
     * using a
     * <a href="DataInput.html#modified-utf-8">modified UTF-8</a>
     * format.
     * <p>
     * The first two bytes are read, starting from the current file
     * pointer, as if by
     * {@code readUnsignedShort}. This value gives the number of
     * following bytes that are in the encoded string, not
     * the length of the resulting string. The following bytes are then
     * interpreted as bytes encoding characters in the modified UTF-8 format
     * and are converted into characters.
     * <p>
     * This method blocks until all the bytes are read, the end of the
     * stream is detected, or an exception is thrown.
     *
     * @return     a Unicode string.
     * @throws     EOFException            if this file reaches the end before
     *               reading all the bytes.
     * @throws     IOException             if an I/O error occurs.
     * @throws     UTFDataFormatException  if the bytes do not represent
     *               valid modified UTF-8 encoding of a Unicode string.
     * @see        java.io.RandomAccessFile#readUnsignedShort()
     */
    public final String readUTF() throws IOException {
        return DataInputStream.readUTF(this);
    }

    /**
     * Writes a {@code boolean} to the file as a one-byte value. The
     * value {@code true} is written out as the value
     * {@code (byte)1}; the value {@code false} is written out
     * as the value {@code (byte)0}. The write starts at
     * the current position of the file pointer.
     *
     * @param      v   a {@code boolean} value to be written.
     * @throws     IOException  if an I/O error occurs.
     */
    public final void writeBoolean(boolean v) throws IOException {
        write(v ? 1 : 0);
    }

    /**
     * Writes a {@code byte} to the file as a one-byte value. The
     * write starts at the current position of the file pointer.
     *
     * @param      v   a {@code byte} value to be written.
     * @throws     IOException  if an I/O error occurs.
     */
    public final void writeByte(int v) throws IOException {
        write(v);
    }

    /**
     * Writes a {@code short} to the file as two bytes, high byte first.
     * The write starts at the current position of the file pointer.
     *
     * @param      v   a {@code short} to be written.
     * @throws     IOException  if an I/O error occurs.
     */
    public final void writeShort(int v) throws IOException {
        buffer[1] = (byte)(v       );
        buffer[0] = (byte)(v >>>  8);
        write(buffer, 0, Short.BYTES);
    }

    /**
     * Writes a {@code char} to the file as a two-byte value, high
     * byte first. The write starts at the current position of the
     * file pointer.
     *
     * @param      v   a {@code char} value to be written.
     * @throws     IOException  if an I/O error occurs.
     */
    public final void writeChar(int v) throws IOException {
        writeShort(v);
    }

    /**
     * Writes an {@code int} to the file as four bytes, high byte first.
     * The write starts at the current position of the file pointer.
     *
     * @param      v   an {@code int} to be written.
     * @throws     IOException  if an I/O error occurs.
     */
    public final void writeInt(int v) throws IOException {
        ByteArray.setInt(buffer, 0, v);
        write(buffer, 0, Integer.BYTES);
        //written += 4;
    }

    /**
     * Writes a {@code long} to the file as eight bytes, high byte first.
     * The write starts at the current position of the file pointer.
     *
     * @param      v   a {@code long} to be written.
     * @throws     IOException  if an I/O error occurs.
     */
    public final void writeLong(long v) throws IOException {
        ByteArray.setLong(buffer, 0, v);
        write(buffer, 0, Long.BYTES);
    }

    /**
     * Converts the float argument to an {@code int} using the
     * {@code floatToIntBits} method in class {@code Float},
     * and then writes that {@code int} value to the file as a
     * four-byte quantity, high byte first. The write starts at the
     * current position of the file pointer.
     *
     * @param      v   a {@code float} value to be written.
     * @throws     IOException  if an I/O error occurs.
     * @see        java.lang.Float#floatToIntBits(float)
     */
    public final void writeFloat(float v) throws IOException {
        ByteArray.setFloat(buffer, 0, v);
        write(buffer, 0, Float.BYTES);
    }

    /**
     * Converts the double argument to a {@code long} using the
     * {@code doubleToLongBits} method in class {@code Double},
     * and then writes that {@code long} value to the file as an
     * eight-byte quantity, high byte first. The write starts at the current
     * position of the file pointer.
     *
     * @param      v   a {@code double} value to be written.
     * @throws     IOException  if an I/O error occurs.
     * @see        java.lang.Double#doubleToLongBits(double)
     */
    public final void writeDouble(double v) throws IOException {
        ByteArray.setDouble(buffer, 0, v);
        write(buffer, 0, Double.BYTES);
    }

    /**
     * Writes the string to the file as a sequence of bytes. Each
     * character in the string is written out, in sequence, by discarding
     * its high eight bits. The write starts at the current position of
     * the file pointer.
     *
     * @param      s   a string of bytes to be written.
     * @throws     IOException  if an I/O error occurs.
     */
    @SuppressWarnings("deprecation")
    public final void writeBytes(String s) throws IOException {
        int len = s.length();
        byte[] b = new byte[len];
        s.getBytes(0, len, b, 0);
        writeBytes(b, 0, len);
    }

    /**
     * Writes a string to the file as a sequence of characters. Each
     * character is written to the data output stream as if by the
     * {@code writeChar} method. The write starts at the current
     * position of the file pointer.
     *
     * @param      s   a {@code String} value to be written.
     * @throws     IOException  if an I/O error occurs.
     * @see        java.io.RandomAccessFile#writeChar(int)
     */
    public final void writeChars(String s) throws IOException {
        int clen = s.length();
        int blen = 2*clen;
        byte[] b = new byte[blen];
        char[] c = new char[clen];
        s.getChars(0, clen, c, 0);
        for (int i = 0, j = 0; i < clen; i++) {
            b[j++] = (byte)(c[i] >>> 8);
            b[j++] = (byte)(c[i] >>> 0);
        }
        writeBytes(b, 0, blen);
    }

    /**
     * Writes a string to the file using
     * <a href="DataInput.html#modified-utf-8">modified UTF-8</a>
     * encoding in a machine-independent manner.
     * <p>
     * First, two bytes are written to the file, starting at the
     * current file pointer, as if by the
     * {@code writeShort} method giving the number of bytes to
     * follow. This value is the number of bytes actually written out,
     * not the length of the string. Following the length, each character
     * of the string is output, in sequence, using the modified UTF-8 encoding
     * for each character.
     *
     * @param      str   a string to be written.
     * @throws     IOException  if an I/O error occurs.
     */
    public final void writeUTF(String str) throws IOException {
        DataOutputStream.writeUTF(str, this);
    }

    private static native void initIDs();

    static {
        initIDs();
        SharedSecrets.setJavaIORandomAccessFileAccess(new JavaIORandomAccessFileAccess()
        {
            // This is for j.u.z.ZipFile.OPEN_DELETE. The O_TEMPORARY flag
            // is only implemented/supported on Windows.
            public RandomAccessFile openAndDelete(File file, String mode)
                throws IOException
            {
                return new RandomAccessFile(file, mode, true);
            }
        });
    }
}
