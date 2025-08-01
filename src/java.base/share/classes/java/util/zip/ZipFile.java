/*
 * Copyright (c) 1995, 2025, Oracle and/or its affiliates. All rights reserved.
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

package java.util.zip;

import java.io.Closeable;
import java.io.InputStream;
import java.io.IOException;
import java.io.EOFException;
import java.io.File;
import java.io.FileDescriptor;
import java.io.RandomAccessFile;
import java.io.UncheckedIOException;
import java.lang.ref.Cleaner.Cleanable;
import java.nio.charset.Charset;
import java.nio.file.InvalidPathException;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.Files;
import java.util.*;
import java.util.function.Consumer;
import java.util.function.IntFunction;
import java.util.jar.JarEntry;
import java.util.jar.JarFile;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;
import jdk.internal.access.JavaUtilZipFileAccess;
import jdk.internal.access.JavaUtilJarAccess;
import jdk.internal.access.SharedSecrets;
import jdk.internal.crac.Core;
import jdk.internal.util.ArraysSupport;
import jdk.internal.util.DecimalDigits;
import jdk.internal.util.OperatingSystem;
import jdk.internal.perf.PerfCounter;
import jdk.internal.ref.CleanerFactory;
import jdk.internal.vm.annotation.Stable;
import sun.nio.cs.UTF_8;
import sun.nio.fs.DefaultFileSystemProvider;
import sun.security.util.SignatureFileVerifier;

import static java.util.zip.ZipConstants64.*;
import static java.util.zip.ZipUtils.*;

/**
 * This class is used to read entries from a ZIP file.
 *
 * <p> Unless otherwise noted, passing a {@code null} argument to a constructor
 * or method in this class will cause a {@link NullPointerException} to be
 * thrown.
 *
 * @apiNote
 * To release resources used by this {@code ZipFile}, the {@link #close()} method
 * should be called explicitly or by try-with-resources. Subclasses are responsible
 * for the cleanup of resources acquired by the subclass. Subclasses that override
 * {@link #finalize()} in order to perform cleanup should be modified to use alternative
 * cleanup mechanisms such as {@link java.lang.ref.Cleaner} and remove the overriding
 * {@code finalize} method.
 *
 * @author      David Connelly
 * @since 1.1
 */
public class ZipFile implements ZipConstants, Closeable {

    private final String filePath;     // ZIP file path
    private final String fileName;     // name of the file
    // Used when decoding entry names and comments
    private final ZipCoder zipCoder;
    private volatile boolean closeRequested;

    // The "resource" used by this ZIP file that needs to be
    // cleaned after use.
    // a) the input streams that need to be closed
    // b) the list of cached Inflater objects
    // c) the "native" source of this ZIP file.
    private final @Stable CleanableResource res;

    private static final int STORED = ZipEntry.STORED;
    private static final int DEFLATED = ZipEntry.DEFLATED;

    /**
     * Mode flag to open a ZIP file for reading.
     */
    public static final int OPEN_READ = 0x1;

    /**
     * Mode flag to open a ZIP file and mark it for deletion.  The file will be
     * deleted some time between the moment that it is opened and the moment
     * that it is closed, but its contents will remain accessible via the
     * {@code ZipFile} object until either the close method is invoked or the
     * virtual machine exits.
     */
    public static final int OPEN_DELETE = 0x4;

    /**
     * Flag to specify whether the Extra ZIP64 validation should be
     * disabled.
     */
    private static final boolean DISABLE_ZIP64_EXTRA_VALIDATION =
            getDisableZip64ExtraFieldValidation();

    /**
     * Opens a ZIP file for reading.
     *
     * <p>The UTF-8 {@link java.nio.charset.Charset charset} is used to
     * decode the entry names and comments.
     *
     * @param name the name of the ZIP file
     * @throws ZipException if a ZIP format error has occurred
     * @throws IOException if an I/O error has occurred
     */
    public ZipFile(String name) throws IOException {
        this(new File(name), OPEN_READ);
    }

    /**
     * Opens a new {@code ZipFile} to read from the specified
     * {@code File} object in the specified mode.  The mode argument
     * must be either {@code OPEN_READ} or {@code OPEN_READ | OPEN_DELETE}.
     *
     * <p>The UTF-8 {@link java.nio.charset.Charset charset} is used to
     * decode the entry names and comments
     *
     * @param file the ZIP file to be opened for reading
     * @param mode the mode in which the file is to be opened
     * @throws ZipException if a ZIP format error has occurred
     * @throws IOException if an I/O error has occurred
     * @throws IllegalArgumentException if the {@code mode} argument is invalid
     * @since 1.3
     */
    public ZipFile(File file, int mode) throws IOException {
        this(file, mode, UTF_8.INSTANCE);
    }

    /**
     * Opens a ZIP file for reading given the specified File object.
     *
     * <p>The UTF-8 {@link java.nio.charset.Charset charset} is used to
     * decode the entry names and comments.
     *
     * @param file the ZIP file to be opened for reading
     * @throws ZipException if a ZIP format error has occurred
     * @throws IOException if an I/O error has occurred
     */
    public ZipFile(File file) throws ZipException, IOException {
        this(file, OPEN_READ);
    }

    /**
     * Opens a new {@code ZipFile} to read from the specified
     * {@code File} object in the specified mode.  The mode argument
     * must be either {@code OPEN_READ} or {@code OPEN_READ | OPEN_DELETE}.
     *
     * @param file the ZIP file to be opened for reading
     * @param mode the mode in which the file is to be opened
     * @param charset
     *        the {@linkplain java.nio.charset.Charset charset} to
     *        be used to decode the ZIP entry name and comment that are not
     *        encoded by using UTF-8 encoding (indicated by entry's general
     *        purpose flag).
     *
     * @throws ZipException if a ZIP format error has occurred
     * @throws IOException if an I/O error has occurred
     *
     * @throws IllegalArgumentException if the {@code mode} argument is invalid
     *
     * @since 1.7
     */
    @SuppressWarnings("this-escape")
    public ZipFile(File file, int mode, Charset charset) throws IOException
    {
        if (((mode & OPEN_READ) == 0) ||
            ((mode & ~(OPEN_READ | OPEN_DELETE)) != 0)) {
            throw new IllegalArgumentException("Illegal mode: 0x"+
                                               Integer.toHexString(mode));
        }
        String name = file.getPath();
        file = new File(name);
        Objects.requireNonNull(charset, "charset");

        this.filePath = name;
        this.fileName = file.getName();
        long t0 = System.nanoTime();

        this.zipCoder = ZipCoder.get(charset);
        this.res = new CleanableResource(this, zipCoder, file, mode);

        PerfCounter.getZipFileOpenTime().addElapsedTimeFrom(t0);
        PerfCounter.getZipFileCount().increment();
    }

    /**
     * Opens a ZIP file for reading.
     *
     * @param name the name of the ZIP file
     * @param charset
     *        the {@linkplain java.nio.charset.Charset charset} to
     *        be used to decode the ZIP entry name and comment that are not
     *        encoded by using UTF-8 encoding (indicated by entry's general
     *        purpose flag).
     *
     * @throws ZipException if a ZIP format error has occurred
     * @throws IOException if an I/O error has occurred
     *
     * @since 1.7
     */
    public ZipFile(String name, Charset charset) throws IOException
    {
        this(new File(name), OPEN_READ, charset);
    }

    /**
     * Opens a ZIP file for reading given the specified File object.
     *
     * @param file the ZIP file to be opened for reading
     * @param charset
     *        The {@linkplain java.nio.charset.Charset charset} to be
     *        used to decode the ZIP entry name and comment (ignored if
     *        the <a href="package-summary.html#lang_encoding"> language
     *        encoding bit</a> of the ZIP entry's general purpose bit
     *        flag is set).
     *
     * @throws ZipException if a ZIP format error has occurred
     * @throws IOException if an I/O error has occurred
     *
     * @since 1.7
     */
    public ZipFile(File file, Charset charset) throws IOException
    {
        this(file, OPEN_READ, charset);
    }

    /**
     * Returns the ZIP file comment. If a comment does not exist or an error is
     * encountered decoding the comment using the charset specified
     * when opening the ZIP file, then {@code null} is returned.
     *
     * @return the comment string for the ZIP file, or null if none
     *
     * @throws IllegalStateException if the ZIP file has been closed
     *
     * @since 1.7
     */
    public String getComment() {
        synchronized (this) {
            ensureOpen();
            if (res.zsrc.comment == null) {
                return null;
            }
            // If there is a problem decoding the byte array which represents
            // the ZIP file comment, return null;
            try {
                return zipCoder.toString(res.zsrc.comment);
            } catch (IllegalArgumentException iae) {
                return null;
            }
        }
    }

    /**
     * Returns the ZIP file entry for the specified name, or null
     * if not found.
     *
     * @param name the name of the entry
     * @return the ZIP file entry, or null if not found
     * @throws IllegalStateException if the ZIP file has been closed
     */
    public ZipEntry getEntry(String name) {
        Objects.requireNonNull(name, "name");
        ZipEntry entry = null;
        synchronized (this) {
            ensureOpen();
            // Look up the name and CEN header position of the entry.
            // The resolved name may include a trailing slash.
            // See Source::getEntryPos for details.
            EntryPos pos = res.zsrc.getEntryPos(name, true, zipCoder);
            if (pos != null) {
                entry = getZipEntry(pos.name, pos.pos);
            }
        }
        return entry;
    }

    /**
     * Returns an input stream for reading the contents of the specified
     * ZIP file entry.
     * <p>
     * Closing this ZIP file will, in turn, close all input streams that
     * have been returned by invocations of this method.
     *
     * @apiNote The {@code InputStream} returned by this method can wrap an
     * {@link java.util.zip.InflaterInputStream InflaterInputStream}, whose
     * {@link java.util.zip.InflaterInputStream#read(byte[], int, int)
     * read(byte[], int, int)} method can modify any element of the output
     * buffer.
     *
     * @param entry the ZIP file entry
     * @return the input stream for reading the contents of the specified
     * ZIP file entry or null if the ZIP file entry does not exist
     * within the ZIP file.
     * @throws ZipException if a ZIP format error has occurred
     * @throws IOException if an I/O error has occurred
     * @throws IllegalStateException if the ZIP file has been closed
     */
    public InputStream getInputStream(ZipEntry entry) throws IOException {
        Objects.requireNonNull(entry, "entry");
        int pos;
        ZipFileInputStream in;
        Source zsrc = res.zsrc;
        Set<InputStream> istreams = res.istreams;
        synchronized (this) {
            ensureOpen();
            if (Objects.equals(lastEntryName, entry.name)) {
                pos = lastEntryPos;
            } else {
                EntryPos entryPos = zsrc.getEntryPos(entry.name, false, zipCoder);
                if (entryPos != null) {
                    pos = entryPos.pos;
                } else {
                    pos = -1;
                }
            }
            if (pos == -1) {
                return null;
            }
            in = new ZipFileInputStream(zsrc.cen, pos);
            switch (CENHOW(zsrc.cen, pos)) {
                case STORED:
                    synchronized (istreams) {
                        istreams.add(in);
                    }
                    return in;
                case DEFLATED:
                    // Inflater likes a bit of slack
                    // MORE: Compute good size for inflater stream:
                    long inputBufSize = CENSIZ(zsrc.cen, pos);
                    if (inputBufSize > 65536 || inputBufSize <= 0) {
                        inputBufSize = 8192;
                    }
                    InputStream is = new ZipFileInflaterInputStream(in, res, (int) inputBufSize);
                    synchronized (istreams) {
                        istreams.add(is);
                    }
                    return is;
                default:
                    throw new ZipException("invalid compression method");
            }
        }
    }

    /**
     * Determines and returns a {@link ZipCoder} to use for decoding
     * name and comment fields of the ZIP entry identified by the {@code pos}
     * in the ZIP file's {@code cen}.
     * <p>
     * A ZIP entry's name and comment fields may be encoded using UTF-8, in
     * which case this method returns a UTF-8 capable {@code ZipCoder}. If the
     * entry doesn't require UTF-8, then this method returns the {@code fallback}
     * {@code ZipCoder}.
     *
     * @param cen the CEN
     * @param pos the ZIP entry's position in CEN
     * @param fallback the fallback ZipCoder to return if the entry doesn't require UTF-8
     */
    private static ZipCoder zipCoderFor(final byte[] cen, final int pos, final ZipCoder fallback) {
        if (fallback.isUTF8()) {
            // the fallback ZipCoder is capable of handling UTF-8,
            // so no need to parse the entry flags to determine if
            // the entry has UTF-8 flag.
            return fallback;
        }
        if ((CENFLG(cen, pos) & USE_UTF8) != 0) {
            // entry requires a UTF-8 ZipCoder
            return ZipCoder.UTF8;
        }
        // entry doesn't require a UTF-8 ZipCoder
        return fallback;
    }

    private static class InflaterCleanupAction implements Runnable {
        private final Inflater inf;
        private final CleanableResource res;

        InflaterCleanupAction(Inflater inf, CleanableResource res) {
            this.inf = inf;
            this.res = res;
        }

        @Override
        public void run() {
            res.releaseInflater(inf);
        }
    }

    private class ZipFileInflaterInputStream extends InflaterInputStream {
        private volatile boolean closeRequested;
        private boolean eof = false;
        private final Cleanable cleanable;

        ZipFileInflaterInputStream(ZipFileInputStream zfin,
                                   CleanableResource res, int inputBufSize) {
            this(zfin, res, res.getInflater(), inputBufSize);
        }

        private ZipFileInflaterInputStream(ZipFileInputStream zfin,
                                           CleanableResource res,
                                           Inflater inf, int inputBufSize) {
            super(zfin, inf, inputBufSize);
            this.cleanable = CleanerFactory.cleaner().register(this,
                    new InflaterCleanupAction(inf, res));
        }

        public void close() throws IOException {
            if (closeRequested)
                return;
            closeRequested = true;
            super.close();
            synchronized (res.istreams) {
                res.istreams.remove(this);
            }
            cleanable.clean();
        }

        // Override fill() method to provide an extra "dummy" byte
        // at the end of the input stream. This is required when
        // using the "nowrap" Inflater option.
        protected void fill() throws IOException {
            if (eof) {
                throw new EOFException("Unexpected end of ZLIB input stream");
            }
            len = in.read(buf, 0, buf.length);
            if (len == -1) {
                buf[0] = 0;
                len = 1;
                eof = true;
            }
            inf.setInput(buf, 0, len);
        }

        public int available() throws IOException {
            if (closeRequested)
                return 0;
            long avail = ((ZipFileInputStream)in).size() - inf.getBytesWritten();
            return (avail > (long) Integer.MAX_VALUE ?
                    Integer.MAX_VALUE : (int) avail);
        }
    }

    /**
     * {@return the path name of the ZIP file}
     */
    public String getName() {
        return filePath;
    }

    /**
     * {@return a string identifying this {@code ZipFile}, for debugging}
     */
    @Override
    public String toString() {
        return this.fileName
                + "@" + Integer.toHexString(System.identityHashCode(this));
    }

    private class ZipEntryIterator<T extends ZipEntry>
            implements Enumeration<T>, Iterator<T> {

        private int i = 0;
        private final int entryCount;

        public ZipEntryIterator(int entryCount) {
            this.entryCount = entryCount;
        }

        @Override
        public boolean hasMoreElements() {
            return hasNext();
        }

        @Override
        public boolean hasNext() {
            return i < entryCount;
        }

        @Override
        public T nextElement() {
            return next();
        }

        @Override
        @SuppressWarnings("unchecked")
        public T next() {
            synchronized (ZipFile.this) {
                ensureOpen();
                if (!hasNext()) {
                    throw new NoSuchElementException();
                }
                // each "entry" has 3 ints in table entries
                int pos = res.zsrc.getEntryPos(i++ * 3);
                return (T)getZipEntry(getEntryName(pos), pos);
            }
        }

        @Override
        public Iterator<T> asIterator() {
            return this;
        }
    }

    /**
     * {@return an enumeration of the ZIP file entries}
     * @throws IllegalStateException if the ZIP file has been closed
     */
    public Enumeration<? extends ZipEntry> entries() {
        synchronized (this) {
            ensureOpen();
            return new ZipEntryIterator<ZipEntry>(res.zsrc.total);
        }
    }

    private Enumeration<JarEntry> jarEntries() {
        synchronized (this) {
            ensureOpen();
            return new ZipEntryIterator<JarEntry>(res.zsrc.total);
        }
    }

    private class EntrySpliterator<T> extends Spliterators.AbstractSpliterator<T> {
        private int index;
        private final int fence;
        private final IntFunction<T> gen;

        EntrySpliterator(int index, int fence, IntFunction<T> gen) {
            super((long)fence,
                  Spliterator.ORDERED | Spliterator.DISTINCT | Spliterator.IMMUTABLE |
                  Spliterator.NONNULL);
            this.index = index;
            this.fence = fence;
            this.gen = gen;
        }

        @Override
        public boolean tryAdvance(Consumer<? super T> action) {
            if (action == null)
                throw new NullPointerException();
            if (index >= 0 && index < fence) {
                synchronized (ZipFile.this) {
                    ensureOpen();
                    action.accept(gen.apply(res.zsrc.getEntryPos(index++ * 3)));
                }
                return true;
            }
            return false;
        }
    }

    /**
     * Returns an ordered {@code Stream} over the ZIP file entries.
     *
     * Entries appear in the {@code Stream} in the order they appear in
     * the central directory of the ZIP file.
     *
     * @return an ordered {@code Stream} of entries in this ZIP file
     * @throws IllegalStateException if the ZIP file has been closed
     * @since 1.8
     */
    public Stream<? extends ZipEntry> stream() {
        synchronized (this) {
            ensureOpen();
            return StreamSupport.stream(new EntrySpliterator<>(0, res.zsrc.total,
                pos -> getZipEntry(getEntryName(pos), pos)), false);
       }
    }

    private String getEntryName(int pos) {
        byte[] cen = res.zsrc.cen;
        int nlen = CENNAM(cen, pos);
        ZipCoder zc = zipCoderFor(cen, pos, zipCoder);
        return zc.toString(cen, pos + CENHDR, nlen);
    }

    /*
     * Returns an ordered {@code Stream} over the ZIP file entry names.
     *
     * Entry names appear in the {@code Stream} in the order they appear in
     * the central directory of the ZIP file.
     *
     * @return an ordered {@code Stream} of entry names in this ZIP file
     * @throws IllegalStateException if the ZIP file has been closed
     * @since 10
     */
    private Stream<String> entryNameStream() {
        synchronized (this) {
            ensureOpen();
            return StreamSupport.stream(
                new EntrySpliterator<>(0, res.zsrc.total, this::getEntryName), false);
        }
    }

    /*
     * Returns an ordered {@code Stream} over the ZIP file entries.
     *
     * Entries appear in the {@code Stream} in the order they appear in
     * the central directory of the jar file.
     *
     * @return an ordered {@code Stream} of entries in this ZIP file
     * @throws IllegalStateException if the ZIP file has been closed
     * @since 10
     */
    private Stream<JarEntry> jarStream() {
        synchronized (this) {
            ensureOpen();
            return StreamSupport.stream(new EntrySpliterator<>(0, res.zsrc.total,
                pos -> (JarEntry)getZipEntry(getEntryName(pos), pos)), false);
        }
    }

    private String lastEntryName;
    private int lastEntryPos;

    /* Check ensureOpen() before invoking this method */
    private ZipEntry getZipEntry(String name, int pos) {
        byte[] cen = res.zsrc.cen;
        ZipEntry e = this instanceof JarFile jarFile
                ? Source.JUJA.entryFor(jarFile, name)
                : new ZipEntry(name);

        e.flag = CENFLG(cen, pos);
        e.xdostime = CENTIM(cen, pos);
        e.crc = CENCRC(cen, pos);
        e.size = CENLEN(cen, pos);
        e.csize = CENSIZ(cen, pos);
        e.method = CENHOW(cen, pos);
        if (CENVEM_FA(cen, pos) == FILE_ATTRIBUTES_UNIX) {
            // read all bits in this field, including sym link attributes
            e.externalFileAttributes = CENATX_PERMS(cen, pos) & 0xFFFF;
        }

        int nlen = CENNAM(cen, pos);
        int elen = CENEXT(cen, pos);
        int clen = CENCOM(cen, pos);

        if (elen != 0) {
            int start = pos + CENHDR + nlen;
            e.setExtra0(Arrays.copyOfRange(cen, start, start + elen), true, false);
        }
        if (clen != 0) {
            int start = pos + CENHDR + nlen + elen;
            ZipCoder zc = zipCoderFor(cen, pos, zipCoder);
            e.comment = zc.toString(cen, start, clen);
        }
        lastEntryName = e.name;
        lastEntryPos = pos;
        return e;
    }

    /**
     * Returns the number of entries in the ZIP file.
     *
     * @return the number of entries in the ZIP file
     * @throws IllegalStateException if the ZIP file has been closed
     */
    public int size() {
        synchronized (this) {
            ensureOpen();
            return res.zsrc.total;
        }
    }

    private static class CleanableResource implements Runnable {
        // The outstanding inputstreams that need to be closed
        final Set<InputStream> istreams;

        // List of cached Inflater objects for decompression
        Deque<Inflater> inflaterCache;

        final Cleanable cleanable;

        Source zsrc;

        CleanableResource(ZipFile zf, ZipCoder zipCoder, File file, int mode) throws IOException {
            assert zipCoder != null : "null ZipCoder";
            this.cleanable = CleanerFactory.cleaner().register(zf, this);
            this.istreams = Collections.newSetFromMap(new WeakHashMap<>());
            this.inflaterCache = new ArrayDeque<>();
            this.zsrc = Source.get(file, (mode & OPEN_DELETE) != 0, zipCoder);
        }

        void clean() {
            cleanable.clean();
        }

        /*
         * Gets an inflater from the list of available inflaters or allocates
         * a new one.
         */
        Inflater getInflater() {
            Inflater inf;
            synchronized (inflaterCache) {
                if ((inf = inflaterCache.poll()) != null) {
                    return inf;
                }
            }
            return new Inflater(true);
        }

        /*
         * Releases the specified inflater to the list of available inflaters.
         */
        void releaseInflater(Inflater inf) {
            Deque<Inflater> inflaters = this.inflaterCache;
            if (inflaters != null) {
                synchronized (inflaters) {
                    // double checked!
                    if (inflaters == this.inflaterCache) {
                        inf.reset();
                        inflaters.add(inf);
                        return;
                    }
                }
            }
            // inflaters cache already closed - just end it.
            inf.end();
        }

        public void run() {
            IOException ioe = null;

            // Release cached inflaters and close the cache first
            Deque<Inflater> inflaters = this.inflaterCache;
            if (inflaters != null) {
                synchronized (inflaters) {
                    // no need to double-check as only one thread gets a
                    // chance to execute run() (Cleaner guarantee)...
                    Inflater inf;
                    while ((inf = inflaters.poll()) != null) {
                        inf.end();
                    }
                    // close inflaters cache
                    this.inflaterCache = null;
                }
            }

            // Close streams, release their inflaters
            if (istreams != null) {
                synchronized (istreams) {
                    if (!istreams.isEmpty()) {
                        InputStream[] copy = istreams.toArray(new InputStream[0]);
                        istreams.clear();
                        for (InputStream is : copy) {
                            try {
                                is.close();
                            } catch (IOException e) {
                                if (ioe == null) ioe = e;
                                else ioe.addSuppressed(e);
                            }
                        }
                    }
                }
            }

            // Release ZIP src
            if (zsrc != null) {
                synchronized (zsrc) {
                    try {
                        Source.release(zsrc);
                        zsrc = null;
                    } catch (IOException e) {
                        if (ioe == null) ioe = e;
                        else ioe.addSuppressed(e);
                    }
                }
            }
            if (ioe != null) {
                throw new UncheckedIOException(ioe);
            }
        }

        public void beforeCheckpoint() {
            if (zsrc != null) {
                synchronized (zsrc) {
                    zsrc.beforeCheckpoint();
                }
            }
        }
    }

    /**
     * Closes the ZIP file.
     *
     * <p> Closing this ZIP file will close all of the input streams
     * previously returned by invocations of the {@link #getInputStream
     * getInputStream} method.
     *
     * @throws IOException if an I/O error has occurred
     */
    public void close() throws IOException {
        if (closeRequested) {
            return;
        }
        closeRequested = true;

        synchronized (this) {
            // Close streams, release their inflaters, release cached inflaters
            // and release ZIP source
            try {
                res.clean();
            } catch (UncheckedIOException ioe) {
                throw ioe.getCause();
            }
        }
    }

    private void ensureOpen() {
        if (closeRequested) {
            throw new IllegalStateException("zip file closed");
        }
        if (res.zsrc == null) {
            throw new IllegalStateException("The object is not initialized.");
        }
    }

    private void ensureOpenOrZipException() throws IOException {
        if (closeRequested) {
            throw new ZipException("ZipFile closed");
        }
    }

    /*
     * Inner class implementing the input stream used to read a
     * (possibly compressed) ZIP file entry.
     */
    private class ZipFileInputStream extends InputStream {
        private volatile boolean closeRequested;
        private   long pos;     // current position within entry data
        private   long startingPos; // Start position for the entry data
        protected long rem;     // number of remaining bytes within entry
        protected long size;    // uncompressed size of this entry

        ZipFileInputStream(byte[] cen, int cenpos) {
            rem = CENSIZ(cen, cenpos);
            size = CENLEN(cen, cenpos);
            pos = CENOFF(cen, cenpos);
            // ZIP64
            if (rem == ZIP64_MAGICVAL || size == ZIP64_MAGICVAL ||
                pos == ZIP64_MAGICVAL) {
                checkZIP64(cen, cenpos);
            }
            // negative for lazy initialization, see getDataOffset();
            pos = - (pos + ZipFile.this.res.zsrc.locpos);
        }

        private void checkZIP64(byte[] cen, int cenpos) {
            int off = cenpos + CENHDR + CENNAM(cen, cenpos);
            int end = off + CENEXT(cen, cenpos);
            while (off + 4 < end) {
                int tag = get16(cen, off);
                int sz = get16(cen, off + 2);
                off += 4;
                if (off + sz > end)         // invalid data
                    break;
                if (tag == EXTID_ZIP64) {
                    if (size == ZIP64_MAGICVAL) {
                        if (sz < 8 || (off + 8) > end)
                            break;
                        size = get64S(cen, off);
                        sz -= 8;
                        off += 8;
                    }
                    if (rem == ZIP64_MAGICVAL) {
                        if (sz < 8 || (off + 8) > end)
                            break;
                        rem = get64S(cen, off);
                        sz -= 8;
                        off += 8;
                    }
                    if (pos == ZIP64_MAGICVAL) {
                        if (sz < 8 || (off + 8) > end)
                            break;
                        pos = get64S(cen, off);
                        sz -= 8;
                        off += 8;
                    }
                    break;
                }
                off += sz;
            }
        }

        /*
         * The ZIP file spec explicitly allows the LOC extra data size to
         * be different from the CEN extra data size. Since we cannot trust
         * the CEN extra data size, we need to read the LOC to determine
         * the entry data offset.
         */
        private long initDataOffset() throws IOException {
            if (pos <= 0) {
                byte[] loc = new byte[LOCHDR];
                pos = -pos;
                int len = ZipFile.this.res.zsrc.readFullyAt(loc, 0, loc.length, pos);
                if (len != LOCHDR) {
                    throw new ZipException("ZipFile error reading zip file");
                }
                if (LOCSIG(loc) != LOCSIG) {
                    throw new ZipException("ZipFile invalid LOC header (bad signature)");
                }
                pos += LOCHDR + LOCNAM(loc) + LOCEXT(loc);
                startingPos = pos; // Save starting position for the entry
            }
            return pos;
        }

        public int read(byte[] b, int off, int len) throws IOException {
            synchronized (ZipFile.this) {
                ensureOpenOrZipException();
                initDataOffset();
                if (rem == 0) {
                    return -1;
                }
                if (len > rem) {
                    len = (int) rem;
                }
                if (len <= 0) {
                    return 0;
                }
                len = ZipFile.this.res.zsrc.readAt(b, off, len, pos);
                if (len > 0) {
                    pos += len;
                    rem -= len;
                }
            }
            if (rem == 0) {
                close();
            }
            return len;
        }

        public int read() throws IOException {
            byte[] b = new byte[1];
            if (read(b, 0, 1) == 1) {
                return b[0] & 0xff;
            } else {
                return -1;
            }
        }

        public long skip(long n) throws IOException {
            synchronized (ZipFile.this) {
                initDataOffset();
                long newPos = pos + n;
                if (n > 0) {
                    // If we overflowed adding the skip value or are moving
                    // past EOF, set the skip value to number of bytes remaining
                    // to reach EOF
                    if (newPos < 0 || n > rem) {
                        n = rem;
                    }
                } else if (newPos < startingPos) {
                    // Tried to position before BOF so set position to the
                    // BOF and return the number of bytes we moved backwards
                    // to reach BOF
                    n = startingPos - pos;
                }
                pos += n;
                rem -= n;
            }
            if (rem == 0) {
                close();
            }
            return n;
        }

        public int available() {
            return rem > Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) rem;
        }

        public long size() {
            return size;
        }

        public void close() {
            if (closeRequested) {
                return;
            }
            closeRequested = true;
            rem = 0;
            synchronized (res.istreams) {
                res.istreams.remove(this);
            }
        }

    }

    /**
     * Returns the names of the META-INF/MANIFEST.MF entry - if exists -
     * and any signature-related files under META-INF. This method is used in
     * JarFile, via SharedSecrets, as an optimization.
     */
    private List<String> getManifestAndSignatureRelatedFiles() {
        synchronized (this) {
            ensureOpen();
            Source zsrc = res.zsrc;
            int[] metanames = zsrc.signatureMetaNames;
            List<String> files = null;
            if (zsrc.manifestPos >= 0) {
                files = new ArrayList<>();
                files.add(getEntryName(zsrc.manifestPos));
            }
            if (metanames != null) {
                if (files == null) {
                    files = new ArrayList<>();
                }
                for (int i = 0; i < metanames.length; i++) {
                    files.add(getEntryName(metanames[i]));
                }
            }
            return files == null ? List.of() : files;
        }
    }

    /**
     * Returns the number of the META-INF/MANIFEST.MF entries, case insensitive.
     * When this number is greater than 1, JarVerifier will treat a file as
     * unsigned.
     */
    private int getManifestNum() {
        synchronized (this) {
            ensureOpen();
            return res.zsrc.manifestNum;
        }
    }

    /**
     * Returns the name of the META-INF/MANIFEST.MF entry, ignoring
     * case. If {@code onlyIfSignatureRelatedFiles} is true, we only return the
     * manifest if there is also at least one signature-related file.
     * This method is used in JarFile, via SharedSecrets, as an optimization
     * when looking up the manifest file.
     */
    private String getManifestName(boolean onlyIfSignatureRelatedFiles) {
        synchronized (this) {
            ensureOpen();
            Source zsrc = res.zsrc;
            int pos = zsrc.manifestPos;
            if (pos >= 0 && (!onlyIfSignatureRelatedFiles || zsrc.signatureMetaNames != null)) {
                return getEntryName(pos);
            }
        }
        return null;
    }

    /**
     * Returns a BitSet where the set bits represents versions found for
     * the given entry name. For performance reasons, the name is looked
     * up only by hashcode, meaning the result is an over-approximation.
     * This method is used in JarFile, via SharedSecrets, as an
     * optimization when looking up potentially versioned entries.
     * Returns an empty BitSet if no versioned entries exist for this
     * name.
     */
    private BitSet getMetaInfVersions(String name) {
        synchronized (this) {
            ensureOpen();
            return res.zsrc.metaVersions.getOrDefault(ZipCoder.hash(name), EMPTY_VERSIONS);
        }
    }

    private static final BitSet EMPTY_VERSIONS = new BitSet();

    /**
     * Returns the value of the System property which indicates whether the
     * Extra ZIP64 validation should be disabled.
     */
    static boolean getDisableZip64ExtraFieldValidation() {
        boolean result;
        String value = System.getProperty("jdk.util.zip.disableZip64ExtraFieldValidation");
        if (value == null) {
            result = false;
        } else {
            result = value.isEmpty() || value.equalsIgnoreCase("true");
        }
        return result;
    }

    private synchronized void beforeCheckpoint() {
        res.beforeCheckpoint();
    }

    static {
        SharedSecrets.setJavaUtilZipFileAccess(
            new JavaUtilZipFileAccess() {
                @Override
                public boolean startsWithLocHeader(ZipFile zip) {
                    return zip.res.zsrc.startsWithLoc;
                }
                @Override
                public List<String> getManifestAndSignatureRelatedFiles(JarFile jar) {
                    return ((ZipFile)jar).getManifestAndSignatureRelatedFiles();
                }
                @Override
                public int getManifestNum(JarFile jar) {
                    return ((ZipFile)jar).getManifestNum();
                }
                @Override
                public String getManifestName(JarFile jar, boolean onlyIfHasSignatureRelatedFiles) {
                    return ((ZipFile)jar).getManifestName(onlyIfHasSignatureRelatedFiles);
                }
                @Override
                public BitSet getMetaInfVersions(JarFile jar, String name) {
                    return ((ZipFile)jar).getMetaInfVersions(name);
                }
                @Override
                public Enumeration<JarEntry> entries(ZipFile zip) {
                    return zip.jarEntries();
                }
                @Override
                public Stream<JarEntry> stream(ZipFile zip) {
                    return zip.jarStream();
                }
                @Override
                public Stream<String> entryNameStream(ZipFile zip) {
                    return zip.entryNameStream();
                }
                @Override
                public int getExternalFileAttributes(ZipEntry ze) {
                    return ze.externalFileAttributes;
                }
                @Override
                public void setExternalFileAttributes(ZipEntry ze, int externalFileAttributes) {
                    ze.externalFileAttributes = externalFileAttributes;
                }
                @Override
                public void beforeCheckpoint(ZipFile zip) {
                    zip.beforeCheckpoint();
                }
            }
        );
    }
    // Represents the resolved name and position of a CEN record
    static record EntryPos(String name, int pos) {}

    // Implementation note: This class is thread safe.
    private static class Source {
        // While this is only used from ZipFile, defining it there would cause
        // a bootstrap cycle that would leave this initialized as null
        private static final JavaUtilJarAccess JUJA = SharedSecrets.javaUtilJarAccess();
        // "META-INF/".length()
        private static final int META_INF_LEN = 9;
        // "META-INF/versions//".length()
        private static final int META_INF_VERSIONS_LEN = 19;
        // CEN size is limited to the maximum array size in the JDK
        private static final int MAX_CEN_SIZE = ArraysSupport.SOFT_MAX_ARRAY_LENGTH;

        private final Key key;               // the key in files

        private int refs = 1;

        private RandomAccessFile zfile;      // zfile of the underlying ZIP file
        private byte[] cen;                  // CEN
        private long locpos;                 // position of first LOC header (usually 0)
        private byte[] comment;              // ZIP file comment
                                             // list of meta entries in META-INF dir
        private int   manifestPos = -1;      // position of the META-INF/MANIFEST.MF, if exists
        private int   manifestNum = 0;       // number of META-INF/MANIFEST.MF, case insensitive
        private int[] signatureMetaNames;    // positions of signature related entries, if such exist
        private Map<Integer, BitSet> metaVersions; // Versions found in META-INF/versions/, by entry name hash
        private final boolean startsWithLoc; // true, if ZIP file starts with LOCSIG (usually true)

        // A Hashmap for all entries.
        //
        // A cen entry of Zip/JAR file. As we have one for every entry in every active Zip/JAR,
        // We might have a lot of these in a typical system. In order to save space we don't
        // keep the name in memory, but merely remember a 32 bit {@code hash} value of the
        // entry name and its offset {@code pos} in the central directory hdeader.
        //
        // private static class Entry {
        //     int hash;       // 32 bit hashcode on name
        //     int next;       // hash chain: index into entries
        //     int pos;        // Offset of central directory file header
        // }
        // private Entry[] entries;             // array of hashed cen entry
        //
        // To reduce the total size of entries further, we use a int[] here to store 3 "int"
        // {@code hash}, {@code next} and {@code pos} for each entry. The entry can then be
        // referred by their index of their positions in the {@code entries}.
        //
        private int[] entries;                  // array of hashed cen entry

        // Checks the entry at offset pos in the CEN, calculates the Entry values as per above,
        // then returns the length of the entry name. Uses the given zipCoder for processing the
        // entry name and the entry comment (if any).
        private int checkAndAddEntry(final int pos, final int index, final ZipCoder zipCoder)
            throws ZipException
        {
            byte[] cen = this.cen;
            if (CENSIG(cen, pos) != CENSIG) {
                zerror("invalid CEN header (bad signature)");
            }
            int method = CENHOW(cen, pos);
            int flag   = CENFLG(cen, pos);
            if ((flag & 1) != 0) {
                zerror("invalid CEN header (encrypted entry)");
            }
            if (method != STORED && method != DEFLATED) {
                zerror("invalid CEN header (bad compression method: " + method + ")");
            }
            int entryPos = pos + CENHDR;
            int nlen = CENNAM(cen, pos);
            int elen = CENEXT(cen, pos);
            int clen = CENCOM(cen, pos);
            int headerSize = CENHDR + nlen + clen + elen;
            // CEN header size + name length + comment length + extra length
            // should not exceed 65,535 bytes per the PKWare APP.NOTE
            // 4.4.10, 4.4.11, & 4.4.12.  Also check that current CEN header will
            // not exceed the length of the CEN array
            if (headerSize > 0xFFFF || pos > cen.length - headerSize) {
                zerror("invalid CEN header (bad header size)");
            }

            if (elen > 0 && !DISABLE_ZIP64_EXTRA_VALIDATION) {
                checkExtraFields(pos, entryPos + nlen, elen);
            } else if (elen == 0 && (CENSIZ(cen, pos) == ZIP64_MAGICVAL
                    || CENLEN(cen, pos) == ZIP64_MAGICVAL
                    || CENOFF(cen, pos) == ZIP64_MAGICVAL
                    || CENDSK(cen, pos) == ZIP64_MAGICCOUNT)) {
                zerror("Invalid CEN header (invalid zip64 extra len size)");
            }

            try {
                int hash = zipCoder.checkedHash(cen, entryPos, nlen);
                int hsh = (hash & 0x7fffffff) % tablelen;
                int next = table[hsh];
                table[hsh] = index;
                // Record the CEN offset and the name hash in our hash cell.
                entries[index] = hash;
                entries[index + 1] = next;
                entries[index + 2] = pos;
                // Validate comment if it exists.
                // If the bytes representing the comment cannot be converted to
                // a String via zcp.toString, an Exception will be thrown
                if (clen > 0) {
                    int start = entryPos + nlen + elen;
                    zipCoder.toString(cen, start, clen);
                }
            } catch (Exception e) {
                zerror("invalid CEN header (bad entry name or comment)");
            }
            return nlen;
        }

        /**
         * Validate the Zip64 Extra block fields
         * @param cenPos The CEN offset for the current Entry
         * @param startingOffset Extra Field starting offset within the CEN
         * @param extraFieldLen Length of this Extra field
         * @throws ZipException  If an error occurs validating the Zip64 Extra
         * block
         */
        private void checkExtraFields(int cenPos, int startingOffset,
                                      int extraFieldLen) throws ZipException {
            // Extra field Length cannot exceed 65,535 bytes per the PKWare
            // APP.note 4.4.11
            if (extraFieldLen > 0xFFFF) {
                zerror("invalid extra field length");
            }
            // CEN Offset where this Extra field ends
            int extraEndOffset = startingOffset + extraFieldLen;
            if (extraEndOffset > cen.length) {
                zerror("Invalid CEN header (extra data field size too long)");
            }
            int currentOffset = startingOffset;
            // Walk through each Extra Header. Each Extra Header Must consist of:
            //       Header ID - 2 bytes
            //       Data Size - 2 bytes:
            while (currentOffset + Integer.BYTES <= extraEndOffset) {
                int tag = get16(cen, currentOffset);
                currentOffset += Short.BYTES;

                int tagBlockSize = get16(cen, currentOffset);
                currentOffset += Short.BYTES;
                long tagBlockEndingOffset = (long)currentOffset + tagBlockSize;

                //  The ending offset for this tag block should not go past the
                //  offset for the end of the extra field
                if (tagBlockEndingOffset > extraEndOffset) {
                    zerror(String.format(
                            "Invalid CEN header (invalid extra data field size for " +
                                    "tag: 0x%04x at %d)",
                            tag, cenPos));
                }

                if (tag == EXTID_ZIP64) {
                    // Get the compressed size;
                    long csize = CENSIZ(cen, cenPos);
                    // Get the uncompressed size;
                    long size = CENLEN(cen, cenPos);
                    // Get the LOC offset
                    long locoff = CENOFF(cen, cenPos);
                    // Get the Disk Number
                    int diskNo = CENDSK(cen, cenPos);

                    checkZip64ExtraFieldValues(currentOffset, tagBlockSize,
                            csize, size, locoff, diskNo);
                }
                currentOffset += tagBlockSize;
            }
        }

        /**
         * Validate the Zip64 Extended Information Extra Field (0x0001) block
         * size; that the uncompressed size, compressed size field and LOC
         * offset fields are not negative. Also make sure the field exists if
         * the CEN header field is set to 0xFFFFFFFF.
         * Note:  As we do not use the Starting disk number field,
         * we will not validate its value
         * @param off the starting offset for the Zip64 field value
         * @param blockSize the size of the Zip64 Extended Extra Field
         * @param csize CEN header compressed size value
         * @param size CEN header uncompressed size value
         * @param locoff CEN header LOC offset
         * @param diskNo CEN header Disk number
         * @throws ZipException if an error occurs
         */
        private void checkZip64ExtraFieldValues(int off, int blockSize, long csize,
                                                long size, long locoff, int diskNo)
                throws ZipException {
            byte[] cen = this.cen;
            // if EXTID_ZIP64 blocksize == 0, which may occur with some older
            // versions of Apache Ant and Commons Compress, validate csize and size
            // to make sure neither field == ZIP64_MAGICVAL
            if (blockSize == 0) {
                if (csize == ZIP64_MAGICVAL || size == ZIP64_MAGICVAL ||
                        locoff == ZIP64_MAGICVAL || diskNo == ZIP64_MAGICCOUNT) {
                    zerror("Invalid CEN header (invalid zip64 extra data field size)");
                }
                // Only validate the EXTID_ZIP64 data if the block size > 0
                return;
            }
            // Validate the Zip64 Extended Information Extra Field (0x0001)
            // length.
            if (!isZip64ExtBlockSizeValid(blockSize, csize, size, locoff, diskNo)) {
                zerror("Invalid CEN header (invalid zip64 extra data field size)");
            }
            // Check the uncompressed size is not negative
            if (size == ZIP64_MAGICVAL) {
                if ( blockSize >= Long.BYTES) {
                    if (get64S(cen, off) < 0) {
                        zerror("Invalid zip64 extra block size value");
                    }
                    off += Long.BYTES;
                    blockSize -= Long.BYTES;
                } else {
                    zerror("Invalid Zip64 extra block, missing size");
                }
            }
            // Check the compressed size is not negative
            if (csize == ZIP64_MAGICVAL) {
                if (blockSize >= Long.BYTES) {
                    if (get64S(cen, off) < 0) {
                        zerror("Invalid zip64 extra block compressed size value");
                    }
                    off += Long.BYTES;
                    blockSize -= Long.BYTES;
                } else {
                    zerror("Invalid Zip64 extra block, missing compressed size");
                }
            }
            // Check the LOC offset is not negative
            if (locoff == ZIP64_MAGICVAL) {
                if (blockSize >= Long.BYTES) {
                    if (get64S(cen, off) < 0) {
                        zerror("Invalid zip64 extra block LOC OFFSET value");
                    }
                    // Note: We do not need to adjust the following fields as
                    // this is the last field we are leveraging
                    // off += Long.BYTES;
                    // blockSize -= Long.BYTES;
                } else {
                    zerror("Invalid Zip64 extra block, missing LOC offset value");
                }
            }
        }

        /**
         * Validate the size and contents of a Zip64 extended information field
         * The order of the Zip64 fields is fixed, but the fields MUST
         * only appear if the corresponding LOC or CEN field is set to 0xFFFF:
         * or 0xFFFFFFFF:
         * Uncompressed Size - 8 bytes
         * Compressed Size   - 8 bytes
         * LOC Header offset - 8 bytes
         * Disk Start Number - 4 bytes
         * See PKWare APP.Note Section 4.5.3 for more details
         *
         * @param blockSize the Zip64 Extended Information Extra Field size
         * @param csize CEN header compressed size value
         * @param size CEN header uncompressed size value
         * @param locoff CEN header LOC offset
         * @param diskNo CEN header Disk number
         * @return true if the extra block size is valid; false otherwise
         */
        private static boolean isZip64ExtBlockSizeValid(int blockSize, long csize,
                                                        long size, long locoff,
                                                        int diskNo) {
            int expectedBlockSize =
                    (csize == ZIP64_MAGICVAL ? Long.BYTES : 0) +
                    (size == ZIP64_MAGICVAL ? Long.BYTES : 0) +
                    (locoff == ZIP64_MAGICVAL ? Long.BYTES : 0) +
                    (diskNo == ZIP64_MAGICCOUNT ? Integer.BYTES : 0);
            return expectedBlockSize == blockSize;

        }
        private int getEntryHash(int index) { return entries[index]; }
        private int getEntryNext(int index) { return entries[index + 1]; }
        private int getEntryPos(int index)  { return entries[index + 2]; }
        private static final int ZIP_ENDCHAIN  = -1;
        private int total;                   // total number of entries
        private int[] table;                 // Hash chain heads: indexes into entries
        private int tablelen;                // number of hash heads

        /**
         * A class representing a key to the Source of a ZipFile.
         * The Key is composed of:
         * - The BasicFileAttributes.fileKey() if available, or the Path of the ZIP file
         * if the fileKey() is not available.
         * - The ZIP file's last modified time (to allow for cases
         * where a ZIP file is re-opened after it has been modified).
         * - The Charset that was provided when constructing the ZipFile instance.
         * The unique combination of these components identifies a Source of a ZipFile.
         */
        private static class Key {
            private final BasicFileAttributes attrs;
            private final File file;
            // the Charset that was provided when constructing the ZipFile instance
            private final Charset charset;

            /**
             * Constructs a {@code Key} to a {@code Source} of a {@code ZipFile}
             *
             * @param file    the ZIP file
             * @param attrs   the attributes of the ZIP file
             * @param charset the Charset that was provided when constructing the ZipFile instance
             */
            public Key(File file, BasicFileAttributes attrs, Charset charset) {
                this.attrs = attrs;
                this.file = file;
                this.charset = charset;
            }

            @Override
            public int hashCode() {
                long t = charset.hashCode();
                t += attrs.lastModifiedTime().toMillis();
                Object fk = attrs.fileKey();
                return Long.hashCode(t) +
                        (fk != null ? fk.hashCode() : file.hashCode());
            }

            @Override
            public boolean equals(Object obj) {
                if (obj instanceof Key key) {
                    if (!charset.equals(key.charset)) {
                        return false;
                    }
                    if (!attrs.lastModifiedTime().equals(key.attrs.lastModifiedTime())) {
                        return false;
                    }
                    Object fk = attrs.fileKey();
                    if (fk != null) {
                        return fk.equals(key.attrs.fileKey());
                    } else {
                        return file.equals(key.file);
                    }
                }
                return false;
            }
        }
        private static final HashMap<Key, Source> files = new HashMap<>();
        /**
         * Use the platform's default file system to avoid
         * issues when the VM is configured to use a custom file system provider.
         */
        private static final java.nio.file.FileSystem builtInFS =
                DefaultFileSystemProvider.theFileSystem();

        static Source get(File file, boolean toDelete, ZipCoder zipCoder) throws IOException {
            final Key key;
            try {
                key = new Key(file,
                        Files.readAttributes(builtInFS.getPath(file.getPath()),
                                BasicFileAttributes.class), zipCoder.charset());
            } catch (InvalidPathException ipe) {
                throw new IOException(ipe);
            }
            Source src;
            synchronized (files) {
                src = files.get(key);
                if (src != null) {
                    src.refs++;
                    return src;
                }
            }
            src = new Source(key, toDelete, zipCoder);

            synchronized (files) {
                Source prev = files.putIfAbsent(key, src);
                if (prev != null) {    // someone else put in first
                    src.close();       // close the newly created one
                    prev.refs++;
                    return prev;
                }
                return src;
            }
        }

        static void release(Source src) throws IOException {
            synchronized (files) {
                if (src != null && --src.refs == 0) {
                    files.remove(src.key);
                    src.close();
                }
            }
        }

        private Source(Key key, boolean toDelete, ZipCoder zipCoder) throws IOException {
            this.key = key;
            if (toDelete) {
                if (OperatingSystem.isWindows()) {
                    this.zfile = SharedSecrets.getJavaIORandomAccessFileAccess()
                                              .openAndDelete(key.file, "r");
                } else {
                    this.zfile = new RandomAccessFile(key.file, "r");
                    key.file.delete();
                }
            } else {
                this.zfile = new RandomAccessFile(key.file, "r");
            }
            try {
                initCEN(-1, zipCoder);
                byte[] buf = new byte[4];
                readFullyAt(buf, 0, 4, 0);
                this.startsWithLoc = (LOCSIG(buf) == LOCSIG);
            } catch (IOException x) {
                try {
                    this.zfile.close();
                } catch (IOException xx) {}
                throw x;
            }
        }

        private void close() throws IOException {
            zfile.close();
            zfile = null;
            cen = null;
            entries = null;
            table = null;
            manifestPos = -1;
            manifestNum = 0;
            signatureMetaNames = null;
            metaVersions = null;
        }

        private static final int BUF_SIZE = 8192;
        private final int readFullyAt(byte[] buf, int off, int len, long pos)
            throws IOException
        {
            synchronized (zfile) {
                zfile.seek(pos);
                int N = len;
                while (N > 0) {
                    int n = Math.min(BUF_SIZE, N);
                    zfile.readFully(buf, off, n);
                    off += n;
                    N -= n;
                }
                return len;
            }
        }

        private final int readAt(byte[] buf, int off, int len, long pos)
            throws IOException
        {
            synchronized (zfile) {
                zfile.seek(pos);
                return zfile.read(buf, off, len);
            }
        }


        private static class End {
            long centot;     // 4 bytes
            long cenlen;     // 4 bytes
            long cenoff;     // 4 bytes
            long endpos;     // 4 bytes
        }

        /*
         * Searches for end of central directory (END) header. The contents of
         * the END header will be read and placed in endbuf. Returns the file
         * position of the END header, otherwise returns -1 if the END header
         * was not found or an error occurred.
         */
        private End findEND() throws IOException {
            long ziplen = zfile.length();
            if (ziplen <= 0)
                zerror("zip file is empty");
            End end = new End();
            byte[] buf = new byte[READBLOCKSZ];
            long minHDR = (ziplen - END_MAXLEN) > 0 ? ziplen - END_MAXLEN : 0;
            long minPos = minHDR - (buf.length - ENDHDR);
            for (long pos = ziplen - buf.length; pos >= minPos; pos -= (buf.length - ENDHDR)) {
                int off = 0;
                if (pos < 0) {
                    // Pretend there are some NUL bytes before start of file
                    off = (int)-pos;
                    Arrays.fill(buf, 0, off, (byte)0);
                }
                int len = buf.length - off;
                if (readFullyAt(buf, off, len, pos + off) != len ) {
                    zerror("zip END header not found");
                }
                // Now scan the block backwards for END header signature
                for (int i = buf.length - ENDHDR; i >= 0; i--) {
                    if (get32(buf, i) == ENDSIG) {
                        // Found ENDSIG header
                        byte[] endbuf = Arrays.copyOfRange(buf, i, i + ENDHDR);
                        end.centot = ENDTOT(endbuf);
                        end.cenlen = ENDSIZ(endbuf);
                        end.cenoff = ENDOFF(endbuf);
                        end.endpos = pos + i;
                        int comlen = ENDCOM(endbuf);
                        if (end.endpos + ENDHDR + comlen != ziplen) {
                            // ENDSIG matched, however the size of file comment in it does
                            // not match the real size. One "common" cause for this problem
                            // is some "extra" bytes are padded at the end of the zipfile.
                            // Let's do some extra verification, we don't care about the
                            // performance in this situation.
                            byte[] sbuf = new byte[4];
                            long cenpos = end.endpos - end.cenlen;
                            long locpos = cenpos - end.cenoff;
                            if  (cenpos < 0 ||
                                 locpos < 0 ||
                                 readFullyAt(sbuf, 0, sbuf.length, cenpos) != 4 ||
                                 get32(sbuf, 0) != CENSIG ||
                                 readFullyAt(sbuf, 0, sbuf.length, locpos) != 4 ||
                                 get32(sbuf, 0) != LOCSIG) {
                                continue;
                            }
                        }
                        if (comlen > 0) {    // this zip file has comlen
                            comment = new byte[comlen];
                            if (readFullyAt(comment, 0, comlen, end.endpos + ENDHDR) != comlen) {
                                zerror("zip comment read failed");
                            }
                        }
                        // must check for a ZIP64 end record; it is always permitted to be present
                        try {
                            byte[] loc64 = new byte[ZIP64_LOCHDR];
                            if (end.endpos < ZIP64_LOCHDR ||
                                readFullyAt(loc64, 0, loc64.length, end.endpos - ZIP64_LOCHDR)
                                != loc64.length || get32(loc64, 0) != ZIP64_LOCSIG) {
                                return end;
                            }
                            long end64pos = ZIP64_LOCOFF(loc64);
                            byte[] end64buf = new byte[ZIP64_ENDHDR];
                            if (readFullyAt(end64buf, 0, end64buf.length, end64pos)
                                != end64buf.length || get32(end64buf, 0) != ZIP64_ENDSIG) {
                                return end;
                            }
                            // end64 candidate found,
                            long cenlen64 = ZIP64_ENDSIZ(end64buf);
                            long cenoff64 = ZIP64_ENDOFF(end64buf);
                            long centot64 = ZIP64_ENDTOT(end64buf);
                            // double-check
                            if (cenlen64 != end.cenlen && end.cenlen != ZIP64_MAGICVAL ||
                                cenoff64 != end.cenoff && end.cenoff != ZIP64_MAGICVAL ||
                                centot64 != end.centot && end.centot != ZIP64_MAGICCOUNT) {
                                return end;
                            }
                            // to use the end64 values
                            end.cenlen = cenlen64;
                            end.cenoff = cenoff64;
                            end.centot = centot64;
                            end.endpos = end64pos;
                        } catch (IOException x) {}    // no ZIP64 loc/end
                        return end;
                    }
                }
            }
            throw new ZipException("zip END header not found");
        }

        // Reads ZIP file central directory.
        private void initCEN(final int knownTotal, final ZipCoder zipCoder) throws IOException {
            // Prefer locals for better performance during startup
            byte[] cen;
            if (knownTotal == -1) {
                End end = findEND();
                if (end.endpos == 0) {
                    locpos = 0;
                    total = 0;
                    entries = new int[0];
                    this.cen = null;
                    return;         // only END header present
                }
                if (end.cenlen > end.endpos)
                    zerror("invalid END header (bad central directory size)");
                long cenpos = end.endpos - end.cenlen;     // position of CEN table
                // Get position of first local file (LOC) header, taking into
                // account that there may be a stub prefixed to the ZIP file.
                locpos = cenpos - end.cenoff;
                if (locpos < 0) {
                    zerror("invalid END header (bad central directory offset)");
                }
                // read in the CEN
                if (end.cenlen > MAX_CEN_SIZE) {
                    zerror("invalid END header (central directory size too large)");
                }
                if (end.centot < 0 || end.centot > end.cenlen / CENHDR) {
                    zerror("invalid END header (total entries count too large)");
                }
                cen = this.cen = new byte[(int)end.cenlen];
                if (readFullyAt(cen, 0, cen.length, cenpos) != end.cenlen) {
                    zerror("read CEN tables failed");
                }
                this.total = Math.toIntExact(end.centot);
            } else {
                cen = this.cen;
                this.total = knownTotal;
            }
            // hash table for entries
            int entriesLength = this.total * 3;
            entries = new int[entriesLength];

            int tablelen = ((total/2) | 1); // Odd -> fewer collisions
            this.tablelen = tablelen;

            int[] table = new int[tablelen];
            this.table = table;

            Arrays.fill(table, ZIP_ENDCHAIN);

            // list for all meta entries
            ArrayList<Integer> signatureNames = null;

            // Iterate through the entries in the central directory
            int idx = 0; // Index into the entries array
            int pos = 0;
            manifestNum = 0;
            int limit = cen.length - CENHDR;
            while (pos <= limit) {
                if (idx >= entriesLength) {
                    // This will only happen if the ZIP file has an incorrect
                    // ENDTOT field, which usually means it contains more than
                    // 65535 entries.
                    initCEN(countCENHeaders(cen), zipCoder);
                    return;
                }

                int entryPos = pos + CENHDR;
                // the ZipCoder for any non-UTF8 entries
                final ZipCoder entryZipCoder = zipCoderFor(cen, pos, zipCoder);
                // Checks the entry and adds values to entries[idx ... idx+2]
                int nlen = checkAndAddEntry(pos, idx, entryZipCoder);
                idx += 3;

                // Adds name to metanames.
                if (isMetaName(cen, entryPos, nlen)) {
                    // nlen is at least META_INF_LENGTH
                    if (isManifestName(entryPos + META_INF_LEN, nlen - META_INF_LEN)) {
                        manifestPos = pos;
                        manifestNum++;
                    } else {
                        if (isSignatureRelated(entryPos, nlen)) {
                            if (signatureNames == null)
                                signatureNames = new ArrayList<>(4);
                            signatureNames.add(pos);
                        }

                        // If this is a versioned entry, parse the version
                        // and store it for later. This optimizes lookup
                        // performance in multi-release jar files
                        int version = getMetaVersion(entryPos + META_INF_LEN, nlen - META_INF_LEN);
                        if (version > 0) {
                            try {
                                // Compute hash code of name from "META-INF/versions/{version)/{name}
                                int prefixLen = META_INF_VERSIONS_LEN + DecimalDigits.stringSize(version);
                                int hashCode = entryZipCoder.checkedHash(cen,
                                        entryPos + prefixLen,
                                        nlen - prefixLen);
                                // Register version for this hash code
                                if (metaVersions == null)
                                    metaVersions = new HashMap<>();
                                metaVersions.computeIfAbsent(hashCode, _ -> new BitSet()).set(version);
                            } catch (Exception e) {
                                zerror("invalid CEN header (bad entry name or comment)");
                            }
                        }
                    }
                }
                // skip to the start of the next entry
                pos = nextEntryPos(pos, entryPos, nlen);
            }

            // Adjust the total entries
            this.total = idx / 3;

            if (signatureNames != null) {
                int len = signatureNames.size();
                signatureMetaNames = new int[len];
                for (int j = 0; j < len; j++) {
                    signatureMetaNames[j] = signatureNames.get(j);
                }
            }
            if (metaVersions == null) {
                metaVersions = Map.of();
            }
            if (pos != cen.length) {
                zerror("invalid CEN header (bad header size)");
            }
        }

        private int nextEntryPos(int pos, int entryPos, int nlen) {
            return entryPos + nlen + CENCOM(cen, pos) + CENEXT(cen, pos);
        }

        private static void zerror(String msg) throws ZipException {
            throw new ZipException(msg);
        }

        /*
         * Returns the resolved name and position of the ZIP cen entry corresponding
         * to the specified entry name, or {@code null} if not found.
         */
        private EntryPos getEntryPos(final String name, final boolean addSlash,
                                     final ZipCoder zipCoder) {
            if (total == 0) {
                return null;
            }

            int hsh = ZipCoder.hash(name);
            int idx = table[(hsh & 0x7fffffff) % tablelen];

            int dirPos = -1; // Position of secondary match "name/"

            // Search down the target hash chain for a entry whose
            // 32 bit hash matches the hashed name.
            while (idx != ZIP_ENDCHAIN) {
                if (getEntryHash(idx) == hsh) {

                    int pos = getEntryPos(idx);
                    int noff = pos + CENHDR;
                    int nlen = CENNAM(cen, pos);

                    final ZipCoder zc = zipCoderFor(cen, pos, zipCoder);
                    // Compare the lookup name with the name encoded in the CEN
                    switch (zc.compare(name, cen, noff, nlen, addSlash)) {
                        case ZipCoder.EXACT_MATCH:
                            // We found an exact match for "name"
                            return new EntryPos(name, pos);
                        case ZipCoder.DIRECTORY_MATCH:
                            // We found the directory "name/"
                            // Track its position, then continue the search for "name"
                            dirPos = pos;
                            break;
                        case ZipCoder.NO_MATCH:
                            // Hash collision, continue searching
                    }
                }
                idx = getEntryNext(idx);
            }
            // Reaching this point means we did not find "name".
            // Return the position of "name/" if we found it
            if (dirPos != -1) {
                return new EntryPos(name + "/", dirPos);
            }
            // No entry found
            return null;
        }

        /**
         * Returns true if the bytes represent a non-directory name
         * beginning with "META-INF/", disregarding ASCII case.
         */
        private static boolean isMetaName(byte[] name, int off, int len) {
            // Use the "oldest ASCII trick in the book":
            // ch | 0x20 == Character.toLowerCase(ch)
            return len > META_INF_LEN       // "META-INF/".length()
                && name[off + len - 1] != '/'  // non-directory
                && (name[off++] | 0x20) == 'm'
                && (name[off++] | 0x20) == 'e'
                && (name[off++] | 0x20) == 't'
                && (name[off++] | 0x20) == 'a'
                && (name[off++]       ) == '-'
                && (name[off++] | 0x20) == 'i'
                && (name[off++] | 0x20) == 'n'
                && (name[off++] | 0x20) == 'f'
                && (name[off]         ) == '/';
        }

        /*
         * Check if the bytes represents a name equals to MANIFEST.MF
         */
        private boolean isManifestName(int off, int len) {
            byte[] name = cen;
            return (len == 11 // "MANIFEST.MF".length()
                    && (name[off++] | 0x20) == 'm'
                    && (name[off++] | 0x20) == 'a'
                    && (name[off++] | 0x20) == 'n'
                    && (name[off++] | 0x20) == 'i'
                    && (name[off++] | 0x20) == 'f'
                    && (name[off++] | 0x20) == 'e'
                    && (name[off++] | 0x20) == 's'
                    && (name[off++] | 0x20) == 't'
                    && (name[off++]       ) == '.'
                    && (name[off++] | 0x20) == 'm'
                    && (name[off]   | 0x20) == 'f');
        }

        private boolean isSignatureRelated(int off, int len) {
            // Only called when isMetaName(name, off, len) is true, which means
            // len is at least META_INF_LENGTH
            // assert isMetaName(name, off, len)
            boolean signatureRelated = false;
            byte[] name = cen;
            if (name[off + len - 3] == '.') {
                // Check if entry ends with .EC and .SF
                int b1 = name[off + len - 2] | 0x20;
                int b2 = name[off + len - 1] | 0x20;
                if ((b1 == 'e' && b2 == 'c') || (b1 == 's' && b2 == 'f')) {
                    signatureRelated = true;
                }
            } else if (name[off + len - 4] == '.') {
                // Check if entry ends with .DSA and .RSA
                int b1 = name[off + len - 3] | 0x20;
                int b2 = name[off + len - 2] | 0x20;
                int b3 = name[off + len - 1] | 0x20;
                if ((b1 == 'r' || b1 == 'd') && b2 == 's' && b3 == 'a') {
                    signatureRelated = true;
                }
            }
            // Above logic must match SignatureFileVerifier.isBlockOrSF
            assert(signatureRelated == SignatureFileVerifier
                .isBlockOrSF(new String(name, off, len, UTF_8.INSTANCE)
                    .toUpperCase(Locale.ENGLISH)));

            // Signature related files must reside directly in META-INF/
            if (signatureRelated && hasSlash(name, off + META_INF_LEN, off + len)) {
                signatureRelated = false;
            }
            return signatureRelated;
        }
        /*
         * Return true if the encoded name contains a '/' within the byte given range
         * This assumes an ASCII-compatible encoding, which is ok here since
         * it is already assumed in isMetaName
         */
        private boolean hasSlash(byte[] name, int start, int end) {
            for (int i = start; i < end; i++) {
                int c = name[i];
                if (c == '/') {
                    return true;
                }
            }
            return false;
        }

        /*
         * If the bytes represents a non-directory name beginning
         * with "versions/", continuing with a positive integer,
         * followed by a '/', then return that integer value.
         * Otherwise, return 0
         */
        private int getMetaVersion(int off, int len) {
            byte[] name = cen;
            int nend = off + len;
            if (!(len > 10                         // "versions//".length()
                    && name[off + len - 1] != '/'  // non-directory
                    && (name[off++] | 0x20) == 'v'
                    && (name[off++] | 0x20) == 'e'
                    && (name[off++] | 0x20) == 'r'
                    && (name[off++] | 0x20) == 's'
                    && (name[off++] | 0x20) == 'i'
                    && (name[off++] | 0x20) == 'o'
                    && (name[off++] | 0x20) == 'n'
                    && (name[off++] | 0x20) == 's'
                    && (name[off++]       ) == '/')) {
                return 0;
            }
            int version = 0;
            while (off < nend) {
                final byte c = name[off++];
                if (c == '/') {
                    return version;
                }
                if (c < '0' || c > '9') {
                    return 0;
                }
                version = version * 10 + c - '0';
                // Check for overflow and leading zeros
                if (version <= 0) {
                    return 0;
                }
            }
            return 0;
        }

        /**
         * Returns the number of CEN headers in a central directory.
         *
         * @param cen copy of the bytes in a ZIP file's central directory
         * @throws ZipException if a CEN header exceeds the length of the CEN array
         */
        private static int countCENHeaders(byte[] cen) throws ZipException {
            int count = 0;
            for (int p = 0; p <= cen.length - CENHDR;) {
                int headerSize = CENHDR + CENNAM(cen, p) + CENEXT(cen, p) + CENCOM(cen, p);
                if (p > cen.length - headerSize) {
                    zerror("invalid CEN header (bad header size)");
                }
                p += headerSize;
                count++;
            }
            return count;
        }

        public void beforeCheckpoint() {
            synchronized (zfile) {
                FileDescriptor fd = null;
                try {
                    fd = zfile.getFD();
                } catch (IOException e) {
                }
                if (fd != null) {
                    Core.getClaimedFDs().claimFd(fd, this, () -> null, fd);
                }
            }
        }
    }
}
