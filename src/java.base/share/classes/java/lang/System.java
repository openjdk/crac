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
package java.lang;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.Console;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.lang.annotation.Annotation;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodType;
import java.lang.module.ModuleDescriptor;
import java.lang.reflect.Executable;
import java.lang.reflect.Method;
import java.net.URI;
import java.nio.channels.Channel;
import java.nio.channels.spi.SelectorProvider;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.Charset;
import java.security.ProtectionDomain;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.Properties;
import java.util.ResourceBundle;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.ScheduledExecutorService;
import java.util.function.Supplier;
import java.util.concurrent.ConcurrentHashMap;
import java.util.stream.Stream;

import jdk.internal.javac.Restricted;
import jdk.internal.loader.NativeLibraries;
import jdk.internal.logger.LoggerFinderLoader.TemporaryLoggerFinder;
import jdk.internal.misc.Blocker;
import jdk.internal.misc.CarrierThreadLocal;
import jdk.internal.util.StaticProperty;
import jdk.internal.module.ModuleBootstrap;
import jdk.internal.module.ServicesCatalog;
import jdk.internal.reflect.CallerSensitive;
import jdk.internal.reflect.Reflection;
import jdk.internal.access.JavaLangAccess;
import jdk.internal.access.SharedSecrets;
import jdk.internal.logger.LoggerFinderLoader;
import jdk.internal.logger.LazyLoggers;
import jdk.internal.logger.LocalizedLoggerWrapper;
import jdk.internal.misc.VM;
import jdk.internal.util.SystemProps;
import jdk.internal.vm.Continuation;
import jdk.internal.vm.ContinuationScope;
import jdk.internal.vm.StackableScope;
import jdk.internal.vm.ThreadContainer;
import jdk.internal.vm.annotation.IntrinsicCandidate;
import jdk.internal.vm.annotation.Stable;
import sun.reflect.annotation.AnnotationType;
import sun.nio.ch.Interruptible;
import sun.nio.cs.UTF_8;

/**
 * The {@code System} class contains several useful class fields
 * and methods. It cannot be instantiated.
 *
 * Among the facilities provided by the {@code System} class
 * are standard input, standard output, and error output streams;
 * access to externally defined properties and environment
 * variables; a means of loading files and libraries; and a utility
 * method for quickly copying a portion of an array.
 *
 * @since   1.0
 */
public final class System {
    /* Register the natives via the static initializer.
     *
     * The VM will invoke the initPhase1 method to complete the initialization
     * of this class separate from <clinit>.
     */
    private static native void registerNatives();
    static {
        registerNatives();
    }

    /** Don't let anyone instantiate this class */
    private System() {
    }

    /**
     * The "standard" input stream. This stream is already
     * open and ready to supply input data. This stream
     * corresponds to keyboard input or another input source specified by
     * the host environment or user. Applications should use the encoding
     * specified by the {@link ##stdin.encoding stdin.encoding} property
     * to convert input bytes to character data.
     *
     * @apiNote
     * The typical approach to read character data is to wrap {@code System.in}
     * within the object that handles character encoding. After this is done,
     * subsequent reading should use only the wrapper object; continuing to
     * operate directly on {@code System.in} results in unspecified behavior.
     * <p>
     * Here are two common examples. Using an {@link java.io.InputStreamReader
     * InputStreamReader}:
     * {@snippet lang=java :
     *     new InputStreamReader(System.in, System.getProperty("stdin.encoding"));
     * }
     * Or using a {@link java.util.Scanner Scanner}:
     * {@snippet lang=java :
     *     new Scanner(System.in, System.getProperty("stdin.encoding"));
     * }
     * <p>
     * For handling interactive input, consider using {@link Console}.
     *
     * @see Console
     * @see ##stdin.encoding stdin.encoding
     */
    public static final InputStream in = null;

    /**
     * The "standard" output stream. This stream is already
     * open and ready to accept output data. Typically this stream
     * corresponds to display output or another output destination
     * specified by the host environment or user. The encoding used
     * in the conversion from characters to bytes is equivalent to
     * {@link ##stdout.encoding stdout.encoding}.
     * <p>
     * For simple stand-alone Java applications, a typical way to write
     * a line of output data is:
     * <blockquote><pre>
     *     System.out.println(data)
     * </pre></blockquote>
     * <p>
     * See the {@code println} methods in class {@code PrintStream}.
     *
     * @see     java.io.PrintStream#println()
     * @see     java.io.PrintStream#println(boolean)
     * @see     java.io.PrintStream#println(char)
     * @see     java.io.PrintStream#println(char[])
     * @see     java.io.PrintStream#println(double)
     * @see     java.io.PrintStream#println(float)
     * @see     java.io.PrintStream#println(int)
     * @see     java.io.PrintStream#println(long)
     * @see     java.io.PrintStream#println(java.lang.Object)
     * @see     java.io.PrintStream#println(java.lang.String)
     * @see     ##stdout.encoding stdout.encoding
     */
    public static final PrintStream out = null;

    /**
     * The "standard" error output stream. This stream is already
     * open and ready to accept output data.
     * <p>
     * Typically this stream corresponds to display output or another
     * output destination specified by the host environment or user. By
     * convention, this output stream is used to display error messages
     * or other information that should come to the immediate attention
     * of a user even if the principal output stream, the value of the
     * variable {@code out}, has been redirected to a file or other
     * destination that is typically not continuously monitored.
     * The encoding used in the conversion from characters to bytes is
     * equivalent to {@link ##stderr.encoding stderr.encoding}.
     *
     * @see     ##stderr.encoding stderr.encoding
     */
    public static final PrintStream err = null;

    // Initial values of System.in and System.err, set in initPhase1().
    private static @Stable InputStream initialIn;
    private static @Stable PrintStream initialErr;

    // `sun.jnu.encoding` if it is not supported. Otherwise null.
    // It is initialized in `initPhase1()` before any charset providers
    // are initialized.
    private static String notSupportedJnuEncoding;

    /**
     * Reassigns the "standard" input stream.
     *
     * @param in the new standard input stream.
     *
     * @since   1.1
     */
    public static void setIn(InputStream in) {
        setIn0(in);
    }

    /**
     * Reassigns the "standard" output stream.
     *
     * @param out the new standard output stream
     *
     * @since   1.1
     */
    public static void setOut(PrintStream out) {
        setOut0(out);
    }

    /**
     * Reassigns the "standard" error output stream.
     *
     * @param err the new standard error output stream.
     *
     * @since   1.1
     */
    public static void setErr(PrintStream err) {
        setErr0(err);
    }

    private static volatile Console cons;

    /**
     * Returns the unique {@link java.io.Console Console} object associated
     * with the current Java virtual machine, if any.
     *
     * @return  The system console, if any, otherwise {@code null}.
     *
     * @since   1.6
     */
     public static Console console() {
         Console c;
         if ((c = cons) == null) {
             synchronized (System.class) {
                 if ((c = cons) == null) {
                     cons = c = SharedSecrets.getJavaIOAccess().console();
                 }
             }
         }
         return c;
     }

    /**
     * Returns the channel inherited from the entity that created this
     * Java virtual machine.
     *
     * This method returns the channel obtained by invoking the
     * {@link java.nio.channels.spi.SelectorProvider#inheritedChannel
     * inheritedChannel} method of the system-wide default
     * {@link java.nio.channels.spi.SelectorProvider} object.
     *
     * <p> In addition to the network-oriented channels described in
     * {@link java.nio.channels.spi.SelectorProvider#inheritedChannel
     * inheritedChannel}, this method may return other kinds of
     * channels in the future.
     *
     * @return  The inherited channel, if any, otherwise {@code null}.
     *
     * @throws  IOException
     *          If an I/O error occurs
     *
     * @since 1.5
     */
    public static Channel inheritedChannel() throws IOException {
        return SelectorProvider.provider().inheritedChannel();
    }

    private static native void setIn0(InputStream in);
    private static native void setOut0(PrintStream out);
    private static native void setErr0(PrintStream err);

    /**
     * Throws {@code UnsupportedOperationException}. Setting a security manager
     * is not supported.
     *
     * @param  sm ignored
     * @throws UnsupportedOperationException always
     * @see #getSecurityManager
     * @deprecated This method originally set
     *       {@linkplain SecurityManager the system-wide Security Manager}.
     *       Setting a Security Manager is no longer supported. There is no
     *       replacement for the Security Manager or this method.
     */
    @Deprecated(since="17", forRemoval=true)
    public static void setSecurityManager(@SuppressWarnings("removal") SecurityManager sm) {
        throw new UnsupportedOperationException(
                 "Setting a Security Manager is not supported");
    }

    /**
     * Returns {@code null}. Setting a security manager is not supported.
     *
     * @return  {@code null}
     * @see     #setSecurityManager
     * @deprecated This method originally returned
     *       {@linkplain SecurityManager the system-wide Security Manager}.
     *       Setting a Security Manager is no longer supported. There is no
     *       replacement for the Security Manager or this method.
     */
    @SuppressWarnings("removal")
    @Deprecated(since="17", forRemoval=true)
    public static SecurityManager getSecurityManager() {
        return null;
    }

    /**
     * Returns the current time in milliseconds.  Note that
     * while the unit of time of the return value is a millisecond,
     * the granularity of the value depends on the underlying
     * operating system and may be larger.  For example, many
     * operating systems measure time in units of tens of
     * milliseconds.
     *
     * <p> See the description of the class {@code Date} for
     * a discussion of slight discrepancies that may arise between
     * "computer time" and coordinated universal time (UTC).
     *
     * @return  the difference, measured in milliseconds, between
     *          the current time and midnight, January 1, 1970 UTC.
     * @see     java.util.Date
     */
    @IntrinsicCandidate
    public static native long currentTimeMillis();

    /**
     * Returns the current value of the running Java Virtual Machine's
     * high-resolution time source, in nanoseconds.
     *
     * This method can only be used to measure elapsed time and is
     * not related to any other notion of system or wall-clock time.
     * The value returned represents nanoseconds since some fixed but
     * arbitrary <i>origin</i> time (perhaps in the future, so values
     * may be negative).  The same origin is used by all invocations of
     * this method in an instance of a Java virtual machine; other
     * virtual machine instances are likely to use a different origin.
     *
     * <p>This method provides nanosecond precision, but not necessarily
     * nanosecond resolution (that is, how frequently the value changes)
     * - no guarantees are made except that the resolution is at least as
     * good as that of {@link #currentTimeMillis()}.
     *
     * <p>Differences in successive calls that span greater than
     * approximately 292 years (2<sup>63</sup> nanoseconds) will not
     * correctly compute elapsed time due to numerical overflow.
     *
     * <p>The values returned by this method become meaningful only when
     * the difference between two such values, obtained within the same
     * instance of a Java virtual machine, is computed.
     *
     * <p>For example, to measure how long some code takes to execute:
     * <pre> {@code
     * long startTime = System.nanoTime();
     * // ... the code being measured ...
     * long elapsedNanos = System.nanoTime() - startTime;}</pre>
     *
     * <p>To compare elapsed time against a timeout, use <pre> {@code
     * if (System.nanoTime() - startTime >= timeoutNanos) ...}</pre>
     * instead of <pre> {@code
     * if (System.nanoTime() >= startTime + timeoutNanos) ...}</pre>
     * because of the possibility of numerical overflow.
     *
     * @crac When the process is restored on another machine or if the machine
     * rebooted the value is updated based on wall-clock time difference,
     * resulting in a loss of accuracy when comparing timestamps obtained
     * before and after checkpoint.
     *
     * @return the current value of the running Java Virtual Machine's
     *         high-resolution time source, in nanoseconds
     * @since 1.5
     */
    @IntrinsicCandidate
    public static native long nanoTime();

    /**
     * Copies an array from the specified source array, beginning at the
     * specified position, to the specified position of the destination array.
     * A subsequence of array components are copied from the source
     * array referenced by {@code src} to the destination array
     * referenced by {@code dest}. The number of components copied is
     * equal to the {@code length} argument. The components at
     * positions {@code srcPos} through
     * {@code srcPos+length-1} in the source array are copied into
     * positions {@code destPos} through
     * {@code destPos+length-1}, respectively, of the destination
     * array.
     * <p>
     * If the {@code src} and {@code dest} arguments refer to the
     * same array object, then the copying is performed as if the
     * components at positions {@code srcPos} through
     * {@code srcPos+length-1} were first copied to a temporary
     * array with {@code length} components and then the contents of
     * the temporary array were copied into positions
     * {@code destPos} through {@code destPos+length-1} of the
     * destination array.
     * <p>
     * If {@code dest} is {@code null}, then a
     * {@code NullPointerException} is thrown.
     * <p>
     * If {@code src} is {@code null}, then a
     * {@code NullPointerException} is thrown and the destination
     * array is not modified.
     * <p>
     * Otherwise, if any of the following is true, an
     * {@code ArrayStoreException} is thrown and the destination is
     * not modified:
     * <ul>
     * <li>The {@code src} argument refers to an object that is not an
     *     array.
     * <li>The {@code dest} argument refers to an object that is not an
     *     array.
     * <li>The {@code src} argument and {@code dest} argument refer
     *     to arrays whose component types are different primitive types.
     * <li>The {@code src} argument refers to an array with a primitive
     *    component type and the {@code dest} argument refers to an array
     *     with a reference component type.
     * <li>The {@code src} argument refers to an array with a reference
     *    component type and the {@code dest} argument refers to an array
     *     with a primitive component type.
     * </ul>
     * <p>
     * Otherwise, if any of the following is true, an
     * {@code IndexOutOfBoundsException} is
     * thrown and the destination is not modified:
     * <ul>
     * <li>The {@code srcPos} argument is negative.
     * <li>The {@code destPos} argument is negative.
     * <li>The {@code length} argument is negative.
     * <li>{@code srcPos+length} is greater than
     *     {@code src.length}, the length of the source array.
     * <li>{@code destPos+length} is greater than
     *     {@code dest.length}, the length of the destination array.
     * </ul>
     * <p>
     * Otherwise, if any actual component of the source array from
     * position {@code srcPos} through
     * {@code srcPos+length-1} cannot be converted to the component
     * type of the destination array by assignment conversion, an
     * {@code ArrayStoreException} is thrown. In this case, let
     * <b><i>k</i></b> be the smallest nonnegative integer less than
     * length such that {@code src[srcPos+}<i>k</i>{@code ]}
     * cannot be converted to the component type of the destination
     * array; when the exception is thrown, source array components from
     * positions {@code srcPos} through
     * {@code srcPos+}<i>k</i>{@code -1}
     * will already have been copied to destination array positions
     * {@code destPos} through
     * {@code destPos+}<i>k</I>{@code -1} and no other
     * positions of the destination array will have been modified.
     * (Because of the restrictions already itemized, this
     * paragraph effectively applies only to the situation where both
     * arrays have component types that are reference types.)
     *
     * @param      src      the source array.
     * @param      srcPos   starting position in the source array.
     * @param      dest     the destination array.
     * @param      destPos  starting position in the destination data.
     * @param      length   the number of array elements to be copied.
     * @throws     IndexOutOfBoundsException  if copying would cause
     *             access of data outside array bounds.
     * @throws     ArrayStoreException  if an element in the {@code src}
     *             array could not be stored into the {@code dest} array
     *             because of a type mismatch.
     * @throws     NullPointerException if either {@code src} or
     *             {@code dest} is {@code null}.
     */
    @IntrinsicCandidate
    public static native void arraycopy(Object src,  int  srcPos,
                                        Object dest, int destPos,
                                        int length);

    /**
     * Returns the same hash code for the given object as
     * would be returned by the default method hashCode(),
     * whether or not the given object's class overrides
     * hashCode().
     * The hash code for the null reference is zero.
     *
     * @param x object for which the hashCode is to be calculated
     * @return  the hashCode
     * @since   1.1
     * @see Object#hashCode
     * @see java.util.Objects#hashCode(Object)
     */
    @IntrinsicCandidate
    public static native int identityHashCode(Object x);

    /**
     * System properties.
     *
     * See {@linkplain #getProperties getProperties} for details.
     */
    private static Properties props;

    /**
     * Determines the current system properties.
     * <p>
     * The current set of system properties for use by the
     * {@link #getProperty(String)} method is returned as a
     * {@code Properties} object. If there is no current set of
     * system properties, a set of system properties is first created and
     * initialized. This set of system properties includes a value
     * for each of the following keys unless the description of the associated
     * value indicates that the value is optional.
     * <table class="striped" style="text-align:left">
     * <caption style="display:none">Shows property keys and associated values</caption>
     * <thead>
     * <tr><th scope="col">Key</th>
     *     <th scope="col">Description of Associated Value</th></tr>
     * </thead>
     * <tbody>
     * <tr><th scope="row">{@systemProperty java.version}</th>
     *     <td>Java Runtime Environment version, which may be interpreted
     *     as a {@link Runtime.Version}</td></tr>
     * <tr><th scope="row">{@systemProperty java.version.date}</th>
     *     <td>Java Runtime Environment version date, in ISO-8601 YYYY-MM-DD
     *     format, which may be interpreted as a {@link
     *     java.time.LocalDate}</td></tr>
     * <tr><th scope="row">{@systemProperty java.vendor}</th>
     *     <td>Java Runtime Environment vendor</td></tr>
     * <tr><th scope="row">{@systemProperty java.vendor.url}</th>
     *     <td>Java vendor URL</td></tr>
     * <tr><th scope="row">{@systemProperty java.vendor.version}</th>
     *     <td>Java vendor version <em>(optional)</em> </td></tr>
     * <tr><th scope="row">{@systemProperty java.home}</th>
     *     <td>Java installation directory</td></tr>
     * <tr><th scope="row">{@systemProperty java.vm.specification.version}</th>
     *     <td>Java Virtual Machine specification version, whose value is the
     *     {@linkplain Runtime.Version#feature feature} element of the
     *     {@linkplain Runtime#version() runtime version}</td></tr>
     * <tr><th scope="row">{@systemProperty java.vm.specification.vendor}</th>
     *     <td>Java Virtual Machine specification vendor</td></tr>
     * <tr><th scope="row">{@systemProperty java.vm.specification.name}</th>
     *     <td>Java Virtual Machine specification name</td></tr>
     * <tr><th scope="row">{@systemProperty java.vm.version}</th>
     *     <td>Java Virtual Machine implementation version which may be
     *     interpreted as a {@link Runtime.Version}</td></tr>
     * <tr><th scope="row">{@systemProperty java.vm.vendor}</th>
     *     <td>Java Virtual Machine implementation vendor</td></tr>
     * <tr><th scope="row">{@systemProperty java.vm.name}</th>
     *     <td>Java Virtual Machine implementation name</td></tr>
     * <tr><th scope="row">{@systemProperty java.specification.version}</th>
     *     <td>Java Runtime Environment specification version, whose value is
     *     the {@linkplain Runtime.Version#feature feature} element of the
     *     {@linkplain Runtime#version() runtime version}</td></tr>
     * <tr><th scope="row">{@systemProperty java.specification.maintenance.version}</th>
     *     <td>Java Runtime Environment specification maintenance version,
     *     may be interpreted as a positive integer <em>(optional, see below)</em></td></tr>
     * <tr><th scope="row">{@systemProperty java.specification.vendor}</th>
     *     <td>Java Runtime Environment specification  vendor</td></tr>
     * <tr><th scope="row">{@systemProperty java.specification.name}</th>
     *     <td>Java Runtime Environment specification  name</td></tr>
     * <tr><th scope="row">{@systemProperty java.class.version}</th>
     *     <td>{@linkplain java.lang.reflect.ClassFileFormatVersion#latest() Latest}
     *     Java class file format version recognized by the Java runtime as {@code "MAJOR.MINOR"}
     *     where {@link java.lang.reflect.ClassFileFormatVersion#major() MAJOR} and {@code MINOR}
     *     are both formatted as decimal integers</td></tr>
     * <tr><th scope="row">{@systemProperty java.class.path}</th>
     *     <td>Java class path  (refer to
     *        {@link ClassLoader#getSystemClassLoader()} for details)</td></tr>
     * <tr><th scope="row">{@systemProperty java.library.path}</th>
     *     <td>List of paths to search when loading libraries</td></tr>
     * <tr><th scope="row">{@systemProperty java.io.tmpdir}</th>
     *     <td>Default temp file path</td></tr>
     * <tr><th scope="row">{@systemProperty os.name}</th>
     *     <td>Operating system name</td></tr>
     * <tr><th scope="row">{@systemProperty os.arch}</th>
     *     <td>Operating system architecture</td></tr>
     * <tr><th scope="row">{@systemProperty os.version}</th>
     *     <td>Operating system version</td></tr>
     * <tr><th scope="row">{@systemProperty file.separator}</th>
     *     <td>File separator ("/" on UNIX)</td></tr>
     * <tr><th scope="row">{@systemProperty path.separator}</th>
     *     <td>Path separator (":" on UNIX)</td></tr>
     * <tr><th scope="row">{@systemProperty line.separator}</th>
     *     <td>Line separator ("\n" on UNIX)</td></tr>
     * <tr><th scope="row">{@systemProperty user.name}</th>
     *     <td>User's account name</td></tr>
     * <tr><th scope="row">{@systemProperty user.home}</th>
     *     <td>User's home directory</td></tr>
     * <tr><th scope="row">{@systemProperty user.dir}</th>
     *     <td>User's current working directory</td></tr>
     * <tr><th scope="row">{@systemProperty native.encoding}</th>
     *     <td>Character encoding name derived from the host environment and
     *     the user's settings. Setting this system property on the command line
     *     has no effect.</td></tr>
     * <tr><th scope="row">{@systemProperty stdin.encoding}</th>
     *     <td>Character encoding name for {@link System#in System.in}.
     *     The Java runtime can be started with the system property set to {@code UTF-8}.
     *     Starting it with the property set to another value results in unspecified behavior.
     * <tr><th scope="row">{@systemProperty stdout.encoding}</th>
     *     <td>Character encoding name for {@link System#out System.out} and
     *     {@link System#console() System.console()}.
     *     The Java runtime can be started with the system property set to {@code UTF-8}.
     *     Starting it with the property set to another value results in unspecified behavior.
     * <tr><th scope="row">{@systemProperty stderr.encoding}</th>
     *     <td>Character encoding name for {@link System#err System.err}.
     *     The Java runtime can be started with the system property set to {@code UTF-8}.
     *     Starting it with the property set to another value results in unspecified behavior.
     * </tbody>
     * </table>
     * <p>
     * The {@code java.specification.maintenance.version} property is
     * defined if the specification implemented by this runtime at the
     * time of its construction had undergone a <a
     * href="https://jcp.org/en/procedures/jcp2#3.6.4">maintenance
     * release</a>. When defined, its value identifies that
     * maintenance release. To indicate the first maintenance release
     * this property will have the value {@code "1"}, to indicate the
     * second maintenance release this property will have the value
     * {@code "2"}, and so on.
     * <p>
     * Multiple paths in a system property value are separated by the path
     * separator character of the platform.
     * <p>
     * Additional locale-related system properties defined by the
     * {@link Locale##default_locale Default Locale} section in the {@code Locale}
     * class description may also be obtained with this method.
     *
     * @apiNote
     * <strong>Changing a standard system property may have unpredictable results
     * unless otherwise specified.</strong>
     * Property values may be cached during initialization or on first use.
     * Setting a standard property after initialization using {@link #getProperties()},
     * {@link #setProperties(Properties)}, {@link #setProperty(String, String)}, or
     * {@link #clearProperty(String)} may not have the desired effect.
     *
     * @crac System properties can be updated on restore from a checkpoint.
     * The application can {@link jdk.crac/jdk.crac.Context#register(jdk.crac.Resource) register}
     * a resource and reload system properties in the
     * {@link jdk.crac/jdk.crac.Resource#afterRestore(jdk.crac.Context) afterRestore method},
     * updating the application.
     *
     * @implNote
     * In addition to the standard system properties, the system
     * properties may include the following keys:
     * <table class="striped">
     * <caption style="display:none">Shows property keys and associated values</caption>
     * <thead>
     * <tr><th scope="col">Key</th>
     *     <th scope="col">Description of Associated Value</th></tr>
     * </thead>
     * <tbody>
     * <tr><th scope="row">{@systemProperty jdk.module.path}</th>
     *     <td>The application module path</td></tr>
     * <tr><th scope="row">{@systemProperty jdk.module.upgrade.path}</th>
     *     <td>The upgrade module path</td></tr>
     * <tr><th scope="row">{@systemProperty jdk.module.main}</th>
     *     <td>The module name of the initial/main module</td></tr>
     * <tr><th scope="row">{@systemProperty jdk.module.main.class}</th>
     *     <td>The main class name of the initial module</td></tr>
     * <tr><th scope="row">{@systemProperty file.encoding}</th>
     *     <td>The name of the default charset, defaults to {@code UTF-8}.
     *     The property may be set on the command line to the value
     *     {@code UTF-8} or {@code COMPAT}. If set on the command line to
     *     the value {@code COMPAT} then the value is replaced with the
     *     value of the {@code native.encoding} property during startup.
     *     Setting the property to a value other than {@code UTF-8} or
     *     {@code COMPAT} results in unspecified behavior.
     *     </td></tr>
     * </tbody>
     * </table>
     *
     * @return     the system properties
     * @see        #setProperties
     * @see        java.util.Properties
     */
    @SuppressWarnings("doclint:reference") // cross-module links
    public static Properties getProperties() {
        return props;
    }

    /**
     * Returns the system-dependent line separator string.  It always
     * returns the same value - the initial value of the {@linkplain
     * #getProperty(String) system property} {@code line.separator}.
     *
     * <p>On UNIX systems, it returns {@code "\n"}; on Microsoft
     * Windows systems it returns {@code "\r\n"}.
     *
     * @return the system-dependent line separator string
     * @since 1.7
     */
    public static String lineSeparator() {
        return lineSeparator;
    }

    private static String lineSeparator;

    /**
     * Sets the system properties to the {@code Properties} argument.
     * <p>
     * The argument becomes the current set of system properties for use
     * by the {@link #getProperty(String)} method. If the argument is
     * {@code null}, then the current set of system properties is
     * forgotten.
     *
     * @apiNote
     * <strong>Changing a standard system property may have unpredictable results
     * unless otherwise specified</strong>.
     * See {@linkplain #getProperties getProperties} for details.
     *
     * @param      props   the new system properties.
     * @see        #getProperties
     * @see        java.util.Properties
     */
    public static void setProperties(Properties props) {
        if (props == null) {
            Map<String, String> tempProps = SystemProps.initProperties();
            VersionProps.init(tempProps);
            props = createProperties(tempProps);
        }
        System.props = props;
    }

    /**
     * Gets the system property indicated by the specified key.
     * <p>
     * If there is no current set of system properties, a set of system
     * properties is first created and initialized in the same manner as
     * for the {@code getProperties} method.
     *
     * @apiNote
     * <strong>Changing a standard system property may have unpredictable results
     * unless otherwise specified</strong>.
     * See {@linkplain #getProperties getProperties} for details.
     *
     * @crac System properties can be updated on restore from a checkpoint.
     * See {@linkplain #getProperties getProperties} for details.
     *
     * @param      key   the name of the system property.
     * @return     the string value of the system property,
     *             or {@code null} if there is no property with that key.
     *
     * @throws     NullPointerException if {@code key} is {@code null}.
     * @throws     IllegalArgumentException if {@code key} is empty.
     * @see        #setProperty
     * @see        java.lang.System#getProperties()
     */
    public static String getProperty(String key) {
        checkKey(key);
        return props.getProperty(key);
    }

    /**
     * Gets the system property indicated by the specified key.
     * <p>
     * If there is no current set of system properties, a set of system
     * properties is first created and initialized in the same manner as
     * for the {@code getProperties} method.
     *
     * @crac System properties can be updated on restore from a checkpoint.
     * See {@linkplain #getProperties getProperties} for details.
     *
     * @param      key   the name of the system property.
     * @param      def   a default value.
     * @return     the string value of the system property,
     *             or the default value if there is no property with that key.
     *
     * @throws     NullPointerException if {@code key} is {@code null}.
     * @throws     IllegalArgumentException if {@code key} is empty.
     * @see        #setProperty
     * @see        java.lang.System#getProperties()
     */
    public static String getProperty(String key, String def) {
        checkKey(key);
        return props.getProperty(key, def);
    }

    /**
     * Sets the system property indicated by the specified key.
     *
     * @apiNote
     * <strong>Changing a standard system property may have unpredictable results
     * unless otherwise specified</strong>.
     * See {@linkplain #getProperties getProperties} for details.
     *
     * @param      key   the name of the system property.
     * @param      value the value of the system property.
     * @return     the previous value of the system property,
     *             or {@code null} if it did not have one.
     *
     * @throws     NullPointerException if {@code key} or
     *             {@code value} is {@code null}.
     * @throws     IllegalArgumentException if {@code key} is empty.
     * @see        #getProperty
     * @see        java.lang.System#getProperty(java.lang.String)
     * @see        java.lang.System#getProperty(java.lang.String, java.lang.String)
     * @since      1.2
     */
    public static String setProperty(String key, String value) {
        checkKey(key);
        return (String) props.setProperty(key, value);
    }

    /**
     * Removes the system property indicated by the specified key.
     *
     * @apiNote
     * <strong>Changing a standard system property may have unpredictable results
     * unless otherwise specified</strong>.
     * See {@linkplain #getProperties getProperties} method for details.
     *
     * @param      key   the name of the system property to be removed.
     * @return     the previous string value of the system property,
     *             or {@code null} if there was no property with that key.
     *
     * @throws     NullPointerException if {@code key} is {@code null}.
     * @throws     IllegalArgumentException if {@code key} is empty.
     * @see        #getProperty
     * @see        #setProperty
     * @see        java.util.Properties
     * @since 1.5
     */
    public static String clearProperty(String key) {
        checkKey(key);
        return (String) props.remove(key);
    }

    private static void checkKey(String key) {
        if (key == null) {
            throw new NullPointerException("key can't be null");
        }
        if (key.isEmpty()) {
            throw new IllegalArgumentException("key can't be empty");
        }
    }

    /**
     * Gets the value of the specified environment variable. An
     * environment variable is a system-dependent external named
     * value.
     *
     * <p><a id="EnvironmentVSSystemProperties"><i>System
     * properties</i> and <i>environment variables</i></a> are both
     * conceptually mappings between names and values.  Both
     * mechanisms can be used to pass user-defined information to a
     * Java process.  Environment variables have a more global effect,
     * because they are visible to all descendants of the process
     * which defines them, not just the immediate Java subprocess.
     * They can have subtly different semantics, such as case
     * insensitivity, on different operating systems.  For these
     * reasons, environment variables are more likely to have
     * unintended side effects.  It is best to use system properties
     * where possible.  Environment variables should be used when a
     * global effect is desired, or when an external system interface
     * requires an environment variable (such as {@code PATH}).
     *
     * <p>On UNIX systems the alphabetic case of {@code name} is
     * typically significant, while on Microsoft Windows systems it is
     * typically not.  For example, the expression
     * {@code System.getenv("FOO").equals(System.getenv("foo"))}
     * is likely to be true on Microsoft Windows.
     *
     * @crac Environment variables can be updated on restore from a checkpoint.
     * The application can {@link jdk.crac/jdk.crac.Context#register(jdk.crac.Resource) register}
     * a resource and reload environment variables in the
     * {@link jdk.crac/jdk.crac.Resource#afterRestore(jdk.crac.Context) afterRestore method},
     * updating the application.
     *
     * @param  name the name of the environment variable
     * @return the string value of the variable, or {@code null}
     *         if the variable is not defined in the system environment
     * @throws NullPointerException if {@code name} is {@code null}
     * @see    #getenv()
     * @see    ProcessBuilder#environment()
     */
    @SuppressWarnings("doclint:reference") // cross-module links
    public static String getenv(String name) {
        return ProcessEnvironment.getenv(name);
    }


    /**
     * Returns an unmodifiable string map view of the current system environment.
     * The environment is a system-dependent mapping from names to
     * values which is passed from parent to child processes.
     *
     * <p>If the system does not support environment variables, an
     * empty map is returned.
     *
     * <p>The returned map will never contain null keys or values.
     * Attempting to query the presence of a null key or value will
     * throw a {@link NullPointerException}.  Attempting to query
     * the presence of a key or value which is not of type
     * {@link String} will throw a {@link ClassCastException}.
     *
     * <p>The returned map and its collection views may not obey the
     * general contract of the {@link Object#equals} and
     * {@link Object#hashCode} methods.
     *
     * <p>The returned map is typically case-sensitive on all platforms.
     *
     * <p>When passing information to a Java subprocess,
     * <a href=#EnvironmentVSSystemProperties>system properties</a>
     * are generally preferred over environment variables.
     *
     * @crac Environment variables can be updated on restore from a checkpoint.
     * The application can {@link jdk.crac/jdk.crac.Context#register(jdk.crac.Resource) register}
     * a resource and reload the environment variables in the
     * {@link jdk.crac/jdk.crac.Resource#afterRestore(jdk.crac.Context) afterRestore method},
     * updating the application.
     *
     * @return the environment as a map of variable names to values
     * @see    #getenv(String)
     * @see    ProcessBuilder#environment()
     * @since  1.5
     */
    @SuppressWarnings("doclint:reference") // cross-module links
    public static java.util.Map<String,String> getenv() {
        return ProcessEnvironment.getenv();
    }

    /**
     * {@code System.Logger} instances log messages that will be
     * routed to the underlying logging framework the {@link System.LoggerFinder
     * LoggerFinder} uses.
     *
     * {@code System.Logger} instances are typically obtained from
     * the {@link java.lang.System System} class, by calling
     * {@link java.lang.System#getLogger(java.lang.String) System.getLogger(loggerName)}
     * or {@link java.lang.System#getLogger(java.lang.String, java.util.ResourceBundle)
     * System.getLogger(loggerName, bundle)}.
     *
     * @see java.lang.System#getLogger(java.lang.String)
     * @see java.lang.System#getLogger(java.lang.String, java.util.ResourceBundle)
     * @see java.lang.System.LoggerFinder
     *
     * @since 9
     */
    public interface Logger {

        /**
         * System {@linkplain Logger loggers} levels.
         *
         * A level has a {@linkplain #getName() name} and {@linkplain
         * #getSeverity() severity}.
         * Level values are {@link #ALL}, {@link #TRACE}, {@link #DEBUG},
         * {@link #INFO}, {@link #WARNING}, {@link #ERROR}, {@link #OFF},
         * by order of increasing severity.
         * <br>
         * {@link #ALL} and {@link #OFF}
         * are simple markers with severities mapped respectively to
         * {@link java.lang.Integer#MIN_VALUE Integer.MIN_VALUE} and
         * {@link java.lang.Integer#MAX_VALUE Integer.MAX_VALUE}.
         * <p>
         * <b>Severity values and Mapping to {@code java.util.logging.Level}.</b>
         * <p>
         * {@linkplain System.Logger.Level System logger levels} are mapped to
         * {@linkplain java.logging/java.util.logging.Level  java.util.logging levels}
         * of corresponding severity.
         * <br>The mapping is as follows:
         * <br><br>
         * <table class="striped">
         * <caption>System.Logger Severity Level Mapping</caption>
         * <thead>
         * <tr><th scope="col">System.Logger Levels</th>
         *     <th scope="col">java.util.logging Levels</th>
         * </thead>
         * <tbody>
         * <tr><th scope="row">{@link Logger.Level#ALL ALL}</th>
         *     <td>{@link java.logging/java.util.logging.Level#ALL ALL}</td>
         * <tr><th scope="row">{@link Logger.Level#TRACE TRACE}</th>
         *     <td>{@link java.logging/java.util.logging.Level#FINER FINER}</td>
         * <tr><th scope="row">{@link Logger.Level#DEBUG DEBUG}</th>
         *     <td>{@link java.logging/java.util.logging.Level#FINE FINE}</td>
         * <tr><th scope="row">{@link Logger.Level#INFO INFO}</th>
         *     <td>{@link java.logging/java.util.logging.Level#INFO INFO}</td>
         * <tr><th scope="row">{@link Logger.Level#WARNING WARNING}</th>
         *     <td>{@link java.logging/java.util.logging.Level#WARNING WARNING}</td>
         * <tr><th scope="row">{@link Logger.Level#ERROR ERROR}</th>
         *     <td>{@link java.logging/java.util.logging.Level#SEVERE SEVERE}</td>
         * <tr><th scope="row">{@link Logger.Level#OFF OFF}</th>
         *     <td>{@link java.logging/java.util.logging.Level#OFF OFF}</td>
         * </tbody>
         * </table>
         *
         * @since 9
         *
         * @see java.lang.System.LoggerFinder
         * @see java.lang.System.Logger
         */
        @SuppressWarnings("doclint:reference") // cross-module links
        public enum Level {

            // for convenience, we're reusing java.util.logging.Level int values
            // the mapping logic in sun.util.logging.PlatformLogger depends
            // on this.
            /**
             * A marker to indicate that all levels are enabled.
             * This level {@linkplain #getSeverity() severity} is
             * {@link Integer#MIN_VALUE}.
             */
            ALL(Integer.MIN_VALUE),  // typically mapped to/from j.u.l.Level.ALL
            /**
             * {@code TRACE} level: usually used to log diagnostic information.
             * This level {@linkplain #getSeverity() severity} is
             * {@code 400}.
             */
            TRACE(400),   // typically mapped to/from j.u.l.Level.FINER
            /**
             * {@code DEBUG} level: usually used to log debug information traces.
             * This level {@linkplain #getSeverity() severity} is
             * {@code 500}.
             */
            DEBUG(500),   // typically mapped to/from j.u.l.Level.FINEST/FINE/CONFIG
            /**
             * {@code INFO} level: usually used to log information messages.
             * This level {@linkplain #getSeverity() severity} is
             * {@code 800}.
             */
            INFO(800),    // typically mapped to/from j.u.l.Level.INFO
            /**
             * {@code WARNING} level: usually used to log warning messages.
             * This level {@linkplain #getSeverity() severity} is
             * {@code 900}.
             */
            WARNING(900), // typically mapped to/from j.u.l.Level.WARNING
            /**
             * {@code ERROR} level: usually used to log error messages.
             * This level {@linkplain #getSeverity() severity} is
             * {@code 1000}.
             */
            ERROR(1000),  // typically mapped to/from j.u.l.Level.SEVERE
            /**
             * A marker to indicate that all levels are disabled.
             * This level {@linkplain #getSeverity() severity} is
             * {@link Integer#MAX_VALUE}.
             */
            OFF(Integer.MAX_VALUE);  // typically mapped to/from j.u.l.Level.OFF

            private final int severity;

            private Level(int severity) {
                this.severity = severity;
            }

            /**
             * Returns the name of this level.
             * @return this level {@linkplain #name()}.
             */
            public final String getName() {
                return name();
            }

            /**
             * Returns the severity of this level.
             * A higher severity means a more severe condition.
             * @return this level severity.
             */
            public final int getSeverity() {
                return severity;
            }
        }

        /**
         * Returns the name of this logger.
         *
         * @return the logger name.
         */
        public String getName();

        /**
         * Checks if a message of the given level would be logged by
         * this logger.
         *
         * @param level the log message level.
         * @return {@code true} if the given log message level is currently
         *         being logged.
         *
         * @throws NullPointerException if {@code level} is {@code null}.
         */
        public boolean isLoggable(Level level);

        /**
         * Logs a message.
         *
         * @implSpec The default implementation for this method calls
         * {@code this.log(level, (ResourceBundle)null, msg, (Object[])null);}
         *
         * @param level the log message level.
         * @param msg the string message (or a key in the message catalog, if
         * this logger is a {@link
         * LoggerFinder#getLocalizedLogger(java.lang.String,
         * java.util.ResourceBundle, java.lang.Module) localized logger});
         * can be {@code null}.
         *
         * @throws NullPointerException if {@code level} is {@code null}.
         */
        public default void log(Level level, String msg) {
            log(level, (ResourceBundle) null, msg, (Object[]) null);
        }

        /**
         * Logs a lazily supplied message.
         *
         * If the logger is currently enabled for the given log message level
         * then a message is logged that is the result produced by the
         * given supplier function.  Otherwise, the supplier is not operated on.
         *
         * @implSpec When logging is enabled for the given level, the default
         * implementation for this method calls
         * {@code this.log(level, (ResourceBundle)null, msgSupplier.get(), (Object[])null);}
         *
         * @param level the log message level.
         * @param msgSupplier a supplier function that produces a message.
         *
         * @throws NullPointerException if {@code level} is {@code null},
         *         or {@code msgSupplier} is {@code null}.
         */
        public default void log(Level level, Supplier<String> msgSupplier) {
            Objects.requireNonNull(msgSupplier);
            if (isLoggable(Objects.requireNonNull(level))) {
                log(level, (ResourceBundle) null, msgSupplier.get(), (Object[]) null);
            }
        }

        /**
         * Logs a message produced from the given object.
         *
         * If the logger is currently enabled for the given log message level then
         * a message is logged that, by default, is the result produced from
         * calling  toString on the given object.
         * Otherwise, the object is not operated on.
         *
         * @implSpec When logging is enabled for the given level, the default
         * implementation for this method calls
         * {@code this.log(level, (ResourceBundle)null, obj.toString(), (Object[])null);}
         *
         * @param level the log message level.
         * @param obj the object to log.
         *
         * @throws NullPointerException if {@code level} is {@code null}, or
         *         {@code obj} is {@code null}.
         */
        public default void log(Level level, Object obj) {
            Objects.requireNonNull(obj);
            if (isLoggable(Objects.requireNonNull(level))) {
                this.log(level, (ResourceBundle) null, obj.toString(), (Object[]) null);
            }
        }

        /**
         * Logs a message associated with a given throwable.
         *
         * @implSpec The default implementation for this method calls
         * {@code this.log(level, (ResourceBundle)null, msg, thrown);}
         *
         * @param level the log message level.
         * @param msg the string message (or a key in the message catalog, if
         * this logger is a {@link
         * LoggerFinder#getLocalizedLogger(java.lang.String,
         * java.util.ResourceBundle, java.lang.Module) localized logger});
         * can be {@code null}.
         * @param thrown a {@code Throwable} associated with the log message;
         *        can be {@code null}.
         *
         * @throws NullPointerException if {@code level} is {@code null}.
         */
        public default void log(Level level, String msg, Throwable thrown) {
            this.log(level, null, msg, thrown);
        }

        /**
         * Logs a lazily supplied message associated with a given throwable.
         *
         * If the logger is currently enabled for the given log message level
         * then a message is logged that is the result produced by the
         * given supplier function.  Otherwise, the supplier is not operated on.
         *
         * @implSpec When logging is enabled for the given level, the default
         * implementation for this method calls
         * {@code this.log(level, (ResourceBundle)null, msgSupplier.get(), thrown);}
         *
         * @param level one of the log message level identifiers.
         * @param msgSupplier a supplier function that produces a message.
         * @param thrown a {@code Throwable} associated with log message;
         *               can be {@code null}.
         *
         * @throws NullPointerException if {@code level} is {@code null}, or
         *                               {@code msgSupplier} is {@code null}.
         */
        public default void log(Level level, Supplier<String> msgSupplier,
                Throwable thrown) {
            Objects.requireNonNull(msgSupplier);
            if (isLoggable(Objects.requireNonNull(level))) {
                this.log(level, null, msgSupplier.get(), thrown);
            }
        }

        /**
         * Logs a message with an optional list of parameters.
         *
         * @implSpec The default implementation for this method calls
         * {@code this.log(level, (ResourceBundle)null, format, params);}
         *
         * @param level one of the log message level identifiers.
         * @param format the string message format in {@link
         * java.text.MessageFormat} format, (or a key in the message
         * catalog, if this logger is a {@link
         * LoggerFinder#getLocalizedLogger(java.lang.String,
         * java.util.ResourceBundle, java.lang.Module) localized logger});
         * can be {@code null}.
         * @param params an optional list of parameters to the message (may be
         * none).
         *
         * @throws NullPointerException if {@code level} is {@code null}.
         */
        public default void log(Level level, String format, Object... params) {
            this.log(level, null, format, params);
        }

        /**
         * Logs a localized message associated with a given throwable.
         *
         * If the given resource bundle is non-{@code null},  the {@code msg}
         * string is localized using the given resource bundle.
         * Otherwise the {@code msg} string is not localized.
         *
         * @param level the log message level.
         * @param bundle a resource bundle to localize {@code msg}; can be
         * {@code null}.
         * @param msg the string message (or a key in the message catalog,
         *            if {@code bundle} is not {@code null}); can be {@code null}.
         * @param thrown a {@code Throwable} associated with the log message;
         *        can be {@code null}.
         *
         * @throws NullPointerException if {@code level} is {@code null}.
         */
        public void log(Level level, ResourceBundle bundle, String msg,
                Throwable thrown);

        /**
         * Logs a message with resource bundle and an optional list of
         * parameters.
         *
         * If the given resource bundle is non-{@code null},  the {@code format}
         * string is localized using the given resource bundle.
         * Otherwise the {@code format} string is not localized.
         *
         * @param level the log message level.
         * @param bundle a resource bundle to localize {@code format}; can be
         * {@code null}.
         * @param format the string message format in {@link
         * java.text.MessageFormat} format, (or a key in the message
         * catalog if {@code bundle} is not {@code null}); can be {@code null}.
         * @param params an optional list of parameters to the message (may be
         * none).
         *
         * @throws NullPointerException if {@code level} is {@code null}.
         */
        public void log(Level level, ResourceBundle bundle, String format,
                Object... params);
    }

    /**
     * The {@code LoggerFinder} service is responsible for creating, managing,
     * and configuring loggers to the underlying framework it uses.
     *
     * A logger finder is a concrete implementation of this class that has a
     * zero-argument constructor and implements the abstract methods defined
     * by this class.
     * The loggers returned from a logger finder are capable of routing log
     * messages to the logging backend this provider supports.
     * A given invocation of the Java Runtime maintains a single
     * system-wide LoggerFinder instance that is loaded as follows:
     * <ul>
     *    <li>First it finds any custom {@code LoggerFinder} provider
     *        using the {@link java.util.ServiceLoader} facility with the
     *        {@linkplain ClassLoader#getSystemClassLoader() system class
     *        loader}.</li>
     *    <li>If no {@code LoggerFinder} provider is found, the system default
     *        {@code LoggerFinder} implementation will be used.</li>
     * </ul>
     * <p>
     * An application can replace the logging backend
     * <i>even when the java.logging module is present</i>, by simply providing
     * and declaring an implementation of the {@link LoggerFinder} service.
     * <p>
     * <b>Default Implementation</b>
     * <p>
     * The system default {@code LoggerFinder} implementation uses
     * {@code java.util.logging} as the backend framework when the
     * {@code java.logging} module is present.
     * It returns a {@linkplain System.Logger logger} instance
     * that will route log messages to a {@link java.logging/java.util.logging.Logger
     * java.util.logging.Logger}. Otherwise, if {@code java.logging} is not
     * present, the default implementation will return a simple logger
     * instance that will route log messages of {@code INFO} level and above to
     * the console ({@code System.err}).
     * <p>
     * <b>Logging Configuration</b>
     * <p>
     * {@linkplain Logger Logger} instances obtained from the
     * {@code LoggerFinder} factory methods are not directly configurable by
     * the application. Configuration is the responsibility of the underlying
     * logging backend, and usually requires using APIs specific to that backend.
     * <p>For the default {@code LoggerFinder} implementation
     * using {@code java.util.logging} as its backend, refer to
     * {@link java.logging/java.util.logging java.util.logging} for logging configuration.
     * For the default {@code LoggerFinder} implementation returning simple loggers
     * when the {@code java.logging} module is absent, the configuration
     * is implementation dependent.
     * <p>
     * Usually an application that uses a logging framework will log messages
     * through a logger facade defined (or supported) by that framework.
     * Applications that wish to use an external framework should log
     * through the facade associated with that framework.
     * <p>
     * A system class that needs to log messages will typically obtain
     * a {@link System.Logger} instance to route messages to the logging
     * framework selected by the application.
     * <p>
     * Libraries and classes that only need loggers to produce log messages
     * should not attempt to configure loggers by themselves, as that
     * would make them dependent from a specific implementation of the
     * {@code LoggerFinder} service.
     * <p>
     * <b>Message Levels and Mapping to backend levels</b>
     * <p>
     * A logger finder is responsible for mapping from a {@code
     * System.Logger.Level} to a level supported by the logging backend it uses.
     * <br>The default LoggerFinder using {@code java.util.logging} as the backend
     * maps {@code System.Logger} levels to
     * {@linkplain java.logging/java.util.logging.Level java.util.logging} levels
     * of corresponding severity - as described in {@link Logger.Level
     * Logger.Level}.
     *
     * @see java.lang.System
     * @see java.lang.System.Logger
     *
     * @since 9
     */
    @SuppressWarnings("doclint:reference") // cross-module links
    public abstract static class LoggerFinder {

        /**
         * Creates a new instance of {@code LoggerFinder}.
         *
         * @implNote It is recommended that a {@code LoggerFinder} service
         *   implementation does not perform any heavy initialization in its
         *   constructor, in order to avoid possible risks of deadlock or class
         *   loading cycles during the instantiation of the service provider.
         */
        protected LoggerFinder() {
        }

        /**
         * Returns an instance of {@link Logger Logger}
         * for the given {@code module}.
         *
         * @param name the name of the logger.
         * @param module the module for which the logger is being requested.
         *
         * @return a {@link Logger logger} suitable for use within the given
         *         module.
         * @throws NullPointerException if {@code name} is {@code null} or
         *        {@code module} is {@code null}.
         */
        public abstract Logger getLogger(String name, Module module);

        /**
         * Returns a localizable instance of {@link Logger Logger}
         * for the given {@code module}.
         * The returned logger will use the provided resource bundle for
         * message localization.
         *
         * @implSpec By default, this method calls {@link
         * #getLogger(java.lang.String, java.lang.Module)
         * this.getLogger(name, module)} to obtain a logger, then wraps that
         * logger in a {@link Logger} instance where all methods that do not
         * take a {@link ResourceBundle} as parameter are redirected to one
         * which does - passing the given {@code bundle} for
         * localization. So for instance, a call to {@link
         * Logger#log(Logger.Level, String) Logger.log(Level.INFO, msg)}
         * will end up as a call to {@link
         * Logger#log(Logger.Level, ResourceBundle, String, Object...)
         * Logger.log(Level.INFO, bundle, msg, (Object[])null)} on the wrapped
         * logger instance.
         * Note however that by default, string messages returned by {@link
         * java.util.function.Supplier Supplier&lt;String&gt;} will not be
         * localized, as it is assumed that such strings are messages which are
         * already constructed, rather than keys in a resource bundle.
         * <p>
         * An implementation of {@code LoggerFinder} may override this method,
         * for example, when the underlying logging backend provides its own
         * mechanism for localizing log messages, then such a
         * {@code LoggerFinder} would be free to return a logger
         * that makes direct use of the mechanism provided by the backend.
         *
         * @param name    the name of the logger.
         * @param bundle  a resource bundle; can be {@code null}.
         * @param module  the module for which the logger is being requested.
         * @return an instance of {@link Logger Logger}  which will use the
         * provided resource bundle for message localization.
         *
         * @throws NullPointerException if {@code name} is {@code null} or
         *         {@code module} is {@code null}.
         */
        public Logger getLocalizedLogger(String name, ResourceBundle bundle,
                                         Module module) {
            return new LocalizedLoggerWrapper<>(getLogger(name, module), bundle);
        }

        /**
         * Returns the {@code LoggerFinder} instance. There is one
         * single system-wide {@code LoggerFinder} instance in
         * the Java Runtime.  See the class specification of how the
         * {@link LoggerFinder LoggerFinder} implementation is located and
         * loaded.
         *
         * @return the {@link LoggerFinder LoggerFinder} instance.
         */
        public static LoggerFinder getLoggerFinder() {
            return accessProvider();
        }


        private static volatile LoggerFinder service;
        static LoggerFinder accessProvider() {
            // We do not need to synchronize: LoggerFinderLoader will
            // always return the same instance, so if we don't have it,
            // just fetch it again.
            LoggerFinder finder = service;
            if (finder == null) {
                finder = LoggerFinderLoader.getLoggerFinder();
                if (finder instanceof TemporaryLoggerFinder) return finder;
                service = finder;
            }
            return finder;
        }

    }


    /**
     * Returns an instance of {@link Logger Logger} for the caller's
     * use.
     *
     * @implSpec
     * Instances returned by this method route messages to loggers
     * obtained by calling {@link LoggerFinder#getLogger(java.lang.String,
     * java.lang.Module) LoggerFinder.getLogger(name, module)}, where
     * {@code module} is the caller's module.
     * In cases where {@code System.getLogger} is called from a context where
     * there is no caller frame on the stack (e.g when called directly
     * from a JNI attached thread), {@code IllegalCallerException} is thrown.
     * To obtain a logger in such a context, use an auxiliary class that will
     * implicitly be identified as the caller, or use the system {@link
     * LoggerFinder#getLoggerFinder() LoggerFinder} to obtain a logger instead.
     * Note that doing the latter may eagerly initialize the underlying
     * logging system.
     *
     * @apiNote
     * This method may defer calling the {@link
     * LoggerFinder#getLogger(java.lang.String, java.lang.Module)
     * LoggerFinder.getLogger} method to create an actual logger supplied by
     * the logging backend, for instance, to allow loggers to be obtained during
     * the system initialization time.
     *
     * @param name the name of the logger.
     * @return an instance of {@link Logger} that can be used by the calling
     *         class.
     * @throws NullPointerException if {@code name} is {@code null}.
     * @throws IllegalCallerException if there is no Java caller frame on the
     *         stack.
     *
     * @since 9
     */
    @CallerSensitive
    public static Logger getLogger(String name) {
        Objects.requireNonNull(name);
        final Class<?> caller = Reflection.getCallerClass();
        if (caller == null) {
            throw new IllegalCallerException("no caller frame");
        }
        return LazyLoggers.getLogger(name, caller.getModule());
    }

    /**
     * Returns a localizable instance of {@link Logger
     * Logger} for the caller's use.
     * The returned logger will use the provided resource bundle for message
     * localization.
     *
     * @implSpec
     * The returned logger will perform message localization as specified
     * by {@link LoggerFinder#getLocalizedLogger(java.lang.String,
     * java.util.ResourceBundle, java.lang.Module)
     * LoggerFinder.getLocalizedLogger(name, bundle, module)}, where
     * {@code module} is the caller's module.
     * In cases where {@code System.getLogger} is called from a context where
     * there is no caller frame on the stack (e.g when called directly
     * from a JNI attached thread), {@code IllegalCallerException} is thrown.
     * To obtain a logger in such a context, use an auxiliary class that
     * will implicitly be identified as the caller, or use the system {@link
     * LoggerFinder#getLoggerFinder() LoggerFinder} to obtain a logger instead.
     * Note that doing the latter may eagerly initialize the underlying
     * logging system.
     *
     * @apiNote
     * This method is intended to be used after the system is fully initialized.
     * This method may trigger the immediate loading and initialization
     * of the {@link LoggerFinder} service, which may cause issues if the
     * Java Runtime is not ready to initialize the concrete service
     * implementation yet.
     * System classes which may be loaded early in the boot sequence and
     * need to log localized messages should create a logger using
     * {@link #getLogger(java.lang.String)} and then use the log methods that
     * take a resource bundle as parameter.
     *
     * @param name    the name of the logger.
     * @param bundle  a resource bundle.
     * @return an instance of {@link Logger} which will use the provided
     * resource bundle for message localization.
     * @throws NullPointerException if {@code name} is {@code null} or
     *         {@code bundle} is {@code null}.
     * @throws IllegalCallerException if there is no Java caller frame on the
     *         stack.
     *
     * @since 9
     */
    @CallerSensitive
    public static Logger getLogger(String name, ResourceBundle bundle) {
        final ResourceBundle rb = Objects.requireNonNull(bundle);
        Objects.requireNonNull(name);
        final Class<?> caller = Reflection.getCallerClass();
        if (caller == null) {
            throw new IllegalCallerException("no caller frame");
        }
        return LoggerFinder.accessProvider()
                .getLocalizedLogger(name, rb, caller.getModule());
    }

    /**
     * Initiates the {@linkplain Runtime##shutdown shutdown sequence} of the Java Virtual
     * Machine. This method initiates the shutdown sequence (if it is not already initiated)
     * and then blocks indefinitely. This method neither returns nor throws an exception;
     * that is, it does not complete either normally or abruptly.
     * <p>
     * The argument serves as a status code. By convention, a nonzero status code
     * indicates abnormal termination.
     * <p>
     * The call {@code System.exit(n)} is effectively equivalent to the call:
     * {@snippet :
     *     Runtime.getRuntime().exit(n)
     * }
     *
     * @implNote
     * The initiation of the shutdown sequence is logged by {@link Runtime#exit(int)}.
     *
     * @param  status exit status.
     * @see    java.lang.Runtime#exit(int)
     */
    public static void exit(int status) {
        Runtime.getRuntime().exit(status);
    }

    /**
     * Runs the garbage collector in the Java Virtual Machine.
     * <p>
     * Calling the {@code gc} method suggests that the Java Virtual Machine
     * expend effort toward recycling unused objects in order to
     * make the memory they currently occupy available for reuse
     * by the Java Virtual Machine.
     * When control returns from the method call, the Java Virtual Machine
     * has made a best effort to reclaim space from all unused objects.
     * There is no guarantee that this effort will recycle any particular
     * number of unused objects, reclaim any particular amount of space, or
     * complete at any particular time, if at all, before the method returns or ever.
     * There is also no guarantee that this effort will determine
     * the change of reachability in any particular number of objects,
     * or that any particular number of {@link java.lang.ref.Reference Reference}
     * objects will be cleared and enqueued.
     *
     * <p>
     * The call {@code System.gc()} is effectively equivalent to the
     * call:
     * <blockquote><pre>
     * Runtime.getRuntime().gc()
     * </pre></blockquote>
     *
     * @see     java.lang.Runtime#gc()
     */
    public static void gc() {
        Runtime.getRuntime().gc();
    }

    /**
     * Runs the finalization methods of any objects pending finalization.
     *
     * Calling this method suggests that the Java Virtual Machine expend
     * effort toward running the {@code finalize} methods of objects
     * that have been found to be discarded but whose {@code finalize}
     * methods have not yet been run. When control returns from the
     * method call, the Java Virtual Machine has made a best effort to
     * complete all outstanding finalizations.
     * <p>
     * The call {@code System.runFinalization()} is effectively
     * equivalent to the call:
     * <blockquote><pre>
     * Runtime.getRuntime().runFinalization()
     * </pre></blockquote>
     *
     * @deprecated Finalization has been deprecated for removal.  See
     * {@link java.lang.Object#finalize} for background information and details
     * about migration options.
     * <p>
     * When running in a JVM in which finalization has been disabled or removed,
     * no objects will be pending finalization, so this method does nothing.
     *
     * @see     java.lang.Runtime#runFinalization()
     * @jls 12.6 Finalization of Class Instances
     */
    @Deprecated(since="18", forRemoval=true)
    @SuppressWarnings("removal")
    public static void runFinalization() {
        Runtime.getRuntime().runFinalization();
    }

    /**
     * Loads the native library specified by the filename argument.  The filename
     * argument must be an absolute path name.
     *
     * If the filename argument, when stripped of any platform-specific library
     * prefix, path, and file extension, indicates a library whose name is,
     * for example, L, and a native library called L is statically linked
     * with the VM, then the JNI_OnLoad_L function exported by the library
     * is invoked rather than attempting to load a dynamic library.
     * A filename matching the argument does not have to exist in the
     * file system.
     * See the <a href="{@docRoot}/../specs/jni/index.html"> JNI Specification</a>
     * for more details.
     *
     * Otherwise, the filename argument is mapped to a native library image in
     * an implementation-dependent manner.
     *
     * <p>
     * The call {@code System.load(name)} is effectively equivalent
     * to the call:
     * <blockquote><pre>
     * Runtime.getRuntime().load(name)
     * </pre></blockquote>
     *
     * @param      filename   the file to load.
     * @throws     UnsatisfiedLinkError  if either the filename is not an
     *             absolute path name, the native library is not statically
     *             linked with the VM, or the library cannot be mapped to
     *             a native library image by the host system.
     * @throws     NullPointerException if {@code filename} is {@code null}
     * @throws     IllegalCallerException if the caller is in a module that
     *             does not have native access enabled.
     *
     * @spec jni/index.html Java Native Interface Specification
     * @see        java.lang.Runtime#load(java.lang.String)
     */
    @CallerSensitive
    @Restricted
    public static void load(String filename) {
        Class<?> caller = Reflection.getCallerClass();
        Reflection.ensureNativeAccess(caller, System.class, "load", false);
        Runtime.getRuntime().load0(caller, filename);
    }

    /**
     * Loads the native library specified by the {@code libname}
     * argument.  The {@code libname} argument must not contain any platform
     * specific prefix, file extension or path. If a native library
     * called {@code libname} is statically linked with the VM, then the
     * JNI_OnLoad_{@code libname} function exported by the library is invoked.
     * See the <a href="{@docRoot}/../specs/jni/index.html"> JNI Specification</a>
     * for more details.
     *
     * Otherwise, the libname argument is loaded from a system library
     * location and mapped to a native library image in an
     * implementation-dependent manner.
     * <p>
     * The call {@code System.loadLibrary(name)} is effectively
     * equivalent to the call
     * <blockquote><pre>
     * Runtime.getRuntime().loadLibrary(name)
     * </pre></blockquote>
     *
     * @param      libname   the name of the library.
     * @throws     UnsatisfiedLinkError if either the libname argument
     *             contains a file path, the native library is not statically
     *             linked with the VM,  or the library cannot be mapped to a
     *             native library image by the host system.
     * @throws     NullPointerException if {@code libname} is {@code null}
     * @throws     IllegalCallerException if the caller is in a module that
     *             does not have native access enabled.
     *
     * @spec jni/index.html Java Native Interface Specification
     * @see        java.lang.Runtime#loadLibrary(java.lang.String)
     */
    @CallerSensitive
    @Restricted
    public static void loadLibrary(String libname) {
        Class<?> caller = Reflection.getCallerClass();
        Reflection.ensureNativeAccess(caller, System.class, "loadLibrary", false);
        Runtime.getRuntime().loadLibrary0(caller, libname);
    }

    /**
     * Maps a library name into a platform-specific string representing
     * a native library.
     *
     * @param      libname the name of the library.
     * @return     a platform-dependent native library name.
     * @throws     NullPointerException if {@code libname} is {@code null}
     * @see        java.lang.System#loadLibrary(java.lang.String)
     * @see        java.lang.ClassLoader#findLibrary(java.lang.String)
     * @since      1.2
     */
    public static native String mapLibraryName(String libname);

    /**
     * Create PrintStream for stdout/err based on encoding.
     */
    private static PrintStream newPrintStream(OutputStream out, String enc) {
        if (enc != null) {
            return new PrintStream(new BufferedOutputStream(out, 128), true,
                                   Charset.forName(enc, UTF_8.INSTANCE));
        }
        return new PrintStream(new BufferedOutputStream(out, 128), true);
    }

    /**
     * Logs an exception/error at initialization time to stdout or stderr.
     *
     * @param printToStderr to print to stderr rather than stdout
     * @param printStackTrace to print the stack trace
     * @param msg the message to print before the exception, can be {@code null}
     * @param e the exception or error
     */
    private static void logInitException(boolean printToStderr,
                                         boolean printStackTrace,
                                         String msg,
                                         Throwable e) {
        if (VM.initLevel() < 1) {
            throw new InternalError("system classes not initialized");
        }
        PrintStream log = (printToStderr) ? err : out;
        if (msg != null) {
            log.println(msg);
        }
        if (printStackTrace) {
            e.printStackTrace(log);
        } else {
            log.println(e);
            for (Throwable suppressed : e.getSuppressed()) {
                log.println("Suppressed: " + suppressed);
            }
            Throwable cause = e.getCause();
            if (cause != null) {
                log.println("Caused by: " + cause);
            }
        }
    }

    /**
     * Create the Properties object from a map - masking out system properties
     * that are not intended for public access.
     */
    private static Properties createProperties(Map<String, String> initialProps) {
        Properties properties = new Properties(initialProps.size());
        for (var entry : initialProps.entrySet()) {
            String prop = entry.getKey();
            switch (prop) {
                // Do not add private system properties to the Properties
                case "sun.nio.MaxDirectMemorySize":
                case "sun.nio.PageAlignDirectMemory":
                    // used by java.lang.Integer.IntegerCache
                case "java.lang.Integer.IntegerCache.high":
                    // used by sun.launcher.LauncherHelper
                case "sun.java.launcher.diag":
                    // used by jdk.internal.loader.ClassLoaders
                case "jdk.boot.class.path.append":
                    break;
                default:
                    properties.put(prop, entry.getValue());
            }
        }
        return properties;
    }

    /**
     * Initialize the system class.  Called after thread initialization.
     */
    private static void initPhase1() {

        // register the shared secrets - do this first, since SystemProps.initProperties
        // might initialize CharsetDecoders that rely on it
        setJavaLangAccess();

        // VM might invoke JNU_NewStringPlatform() to set those encoding
        // sensitive properties (user.home, user.name, boot.class.path, etc.)
        // during "props" initialization.
        // The charset is initialized in System.c and does not depend on the Properties.
        Map<String, String> tempProps = SystemProps.initProperties();
        VersionProps.init(tempProps);

        // There are certain system configurations that may be controlled by
        // VM options such as the maximum amount of direct memory and
        // Integer cache size used to support the object identity semantics
        // of autoboxing.  Typically, the library will obtain these values
        // from the properties set by the VM.  If the properties are for
        // internal implementation use only, these properties should be
        // masked from the system properties.
        //
        // Save a private copy of the system properties object that
        // can only be accessed by the internal implementation.
        VM.saveProperties(tempProps);
        props = createProperties(tempProps);

        // Check if sun.jnu.encoding is supported. If not, replace it with UTF-8.
        var jnuEncoding = props.getProperty("sun.jnu.encoding");
        if (jnuEncoding == null || !Charset.isSupported(jnuEncoding)) {
            notSupportedJnuEncoding = jnuEncoding == null ? "null" : jnuEncoding;
            props.setProperty("sun.jnu.encoding", "UTF-8");
        }

        StaticProperty.javaHome();          // Load StaticProperty to cache the property values

        lineSeparator = props.getProperty("line.separator");

        FileInputStream fdIn = new In(FileDescriptor.in);
        FileOutputStream fdOut = new Out(FileDescriptor.out);
        FileOutputStream fdErr = new Out(FileDescriptor.err);
        initialIn = new BufferedInputStream(fdIn);
        setIn0(initialIn);
        // stdout/err.encoding are set when the VM is associated with the terminal,
        // thus they are equivalent to Console.charset(), otherwise the encodings
        // of those properties default to native.encoding
        setOut0(newPrintStream(fdOut, props.getProperty("stdout.encoding")));
        initialErr = newPrintStream(fdErr, props.getProperty("stderr.encoding"));
        setErr0(initialErr);

        // Setup Java signal handlers for HUP, TERM, and INT (where available).
        Terminator.setup();

        // Initialize any miscellaneous operating system settings that need to be
        // set for the class libraries. Currently this is no-op everywhere except
        // for Windows where the process-wide error mode is set before the java.io
        // classes are used.
        VM.initializeOSEnvironment();

        // start Finalizer and Reference Handler threads
        SharedSecrets.getJavaLangRefAccess().startThreads();

        // system properties, java.lang and other core classes are now initialized
        VM.initLevel(1);
    }

    /**
     * System.in.
     */
    private static class In extends FileInputStream {
        In(FileDescriptor fd) {
            super(fd);
        }

        @Override
        public int read() throws IOException {
            boolean attempted = Blocker.begin();
            try {
                return super.read();
            } finally {
                Blocker.end(attempted);
            }
        }

        @Override
        public int read(byte[] b) throws IOException {
            boolean attempted = Blocker.begin();
            try {
                return super.read(b);
            } finally {
                Blocker.end(attempted);
            }
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            boolean attempted = Blocker.begin();
            try {
                return super.read(b, off, len);
            } finally {
                Blocker.end(attempted);
            }
        }
    }

    /**
     * System.out/System.err wrap this output stream.
     */
    private static class Out extends FileOutputStream {
        Out(FileDescriptor fd) {
            super(fd);
        }

        @Override
        public void write(int b) throws IOException {
            boolean attempted = Blocker.begin();
            try {
                super.write(b);
            } finally {
                Blocker.end(attempted);
            }
        }

        @Override
        public void write(byte[] b) throws IOException {
            boolean attempted = Blocker.begin();
            try {
                super.write(b);
            } finally {
                Blocker.end(attempted);
            }
        }

        @Override
        public void write(byte[] b, int off, int len) throws IOException {
            boolean attempted = Blocker.begin();
            try {
                super.write(b, off, len);
            } finally {
                Blocker.end(attempted);
            }
        }
    }

    // @see #initPhase2()
    static ModuleLayer bootLayer;

    /*
     * Invoked by VM.  Phase 2 module system initialization.
     * Only classes in java.base can be loaded in this phase.
     *
     * @param printToStderr print exceptions to stderr rather than stdout
     * @param printStackTrace print stack trace when exception occurs
     *
     * @return JNI_OK for success, JNI_ERR for failure
     */
    private static int initPhase2(boolean printToStderr, boolean printStackTrace) {

        try {
            bootLayer = ModuleBootstrap.boot();
        } catch (Exception | Error e) {
            logInitException(printToStderr, printStackTrace,
                             "Error occurred during initialization of boot layer", e);
            return -1; // JNI_ERR
        }

        // module system initialized
        VM.initLevel(2);

        return 0; // JNI_OK
    }

    /*
     * Invoked by VM.  Phase 3 is the final system initialization:
     * 1. set system class loader
     * 2. set TCCL
     *
     * This method must be called after the module system initialization.
     */
    private static void initPhase3() {

        // Emit a warning if java.io.tmpdir is set via the command line
        // to a directory that doesn't exist
        if (SystemProps.isBadIoTmpdir()) {
            System.err.println("WARNING: java.io.tmpdir directory does not exist");
        }

        String smProp = System.getProperty("java.security.manager");
        if (smProp != null) {
            switch (smProp) {
                case "disallow":
                    break;
                case "allow":
                case "":
                case "default":
                default:
                    throw new Error("A command line option has attempted to allow or enable the Security Manager."
                            + " Enabling a Security Manager is not supported.");
            }
        }

        // Emit a warning if `sun.jnu.encoding` is not supported.
        if (notSupportedJnuEncoding != null) {
            System.err.println(
                    "WARNING: The encoding of the underlying platform's" +
                    " file system is not supported: " +
                    notSupportedJnuEncoding);
        }

        // initializing the system class loader
        VM.initLevel(3);

        // system class loader initialized
        ClassLoader scl = ClassLoader.initSystemClassLoader();

        // set TCCL
        Thread.currentThread().setContextClassLoader(scl);

        // system is fully initialized
        VM.initLevel(4);
    }

    private static void setJavaLangAccess() {
        // Allow privileged classes outside of java.lang
        SharedSecrets.setJavaLangAccess(new JavaLangAccess() {
            public List<Method> getDeclaredPublicMethods(Class<?> klass, String name, Class<?>... parameterTypes) {
                return klass.getDeclaredPublicMethods(name, parameterTypes);
            }
            public Method findMethod(Class<?> klass, boolean publicOnly, String name, Class<?>... parameterTypes) {
                return klass.findMethod(publicOnly, name, parameterTypes);
            }
            public jdk.internal.reflect.ConstantPool getConstantPool(Class<?> klass) {
                return klass.getConstantPool();
            }
            public boolean casAnnotationType(Class<?> klass, AnnotationType oldType, AnnotationType newType) {
                return klass.casAnnotationType(oldType, newType);
            }
            public AnnotationType getAnnotationType(Class<?> klass) {
                return klass.getAnnotationType();
            }
            public Map<Class<? extends Annotation>, Annotation> getDeclaredAnnotationMap(Class<?> klass) {
                return klass.getDeclaredAnnotationMap();
            }
            public byte[] getRawClassAnnotations(Class<?> klass) {
                return klass.getRawAnnotations();
            }
            public byte[] getRawClassTypeAnnotations(Class<?> klass) {
                return klass.getRawTypeAnnotations();
            }
            public byte[] getRawExecutableTypeAnnotations(Executable executable) {
                return Class.getExecutableTypeAnnotationBytes(executable);
            }
            public <E extends Enum<E>>
            E[] getEnumConstantsShared(Class<E> klass) {
                return klass.getEnumConstantsShared();
            }
            public int classFileVersion(Class<?> clazz) {
                return clazz.getClassFileVersion();
            }
            public void blockedOn(Interruptible b) {
                Thread.currentThread().blockedOn(b);
            }
            public void registerShutdownHook(int slot, boolean registerShutdownInProgress, Runnable hook) {
                Shutdown.add(slot, registerShutdownInProgress, hook);
            }
            @SuppressWarnings("removal")
            public void invokeFinalize(Object o) throws Throwable {
                o.finalize();
            }
            public ConcurrentHashMap<?, ?> createOrGetClassLoaderValueMap(ClassLoader cl) {
                return cl.createOrGetClassLoaderValueMap();
            }
            public Class<?> defineClass(ClassLoader loader, String name, byte[] b, ProtectionDomain pd, String source) {
                return ClassLoader.defineClass1(loader, name, b, 0, b.length, pd, source);
            }
            public Class<?> defineClass(ClassLoader loader, Class<?> lookup, String name, byte[] b, ProtectionDomain pd,
                                        boolean initialize, int flags, Object classData) {
                return ClassLoader.defineClass0(loader, lookup, name, b, 0, b.length, pd, initialize, flags, classData);
            }
            public Class<?> findBootstrapClassOrNull(String name) {
                return ClassLoader.findBootstrapClassOrNull(name);
            }
            public Package definePackage(ClassLoader cl, String name, Module module) {
                return cl.definePackage(name, module);
            }
            public Module defineModule(ClassLoader loader,
                                       ModuleDescriptor descriptor,
                                       URI uri) {
                return new Module(null, loader, descriptor, uri);
            }
            public Module defineUnnamedModule(ClassLoader loader) {
                return new Module(loader);
            }
            public void addReads(Module m1, Module m2) {
                m1.implAddReads(m2);
            }
            public void addReadsAllUnnamed(Module m) {
                m.implAddReadsAllUnnamed();
            }
            public void addExports(Module m, String pn) {
                m.implAddExports(pn);
            }
            public void addExports(Module m, String pn, Module other) {
                m.implAddExports(pn, other);
            }
            public void addExportsToAllUnnamed(Module m, String pn) {
                m.implAddExportsToAllUnnamed(pn);
            }
            public void addOpens(Module m, String pn, Module other) {
                m.implAddOpens(pn, other);
            }
            public void addOpensToAllUnnamed(Module m, String pn) {
                m.implAddOpensToAllUnnamed(pn);
            }
            public void addUses(Module m, Class<?> service) {
                m.implAddUses(service);
            }
            public boolean isReflectivelyExported(Module m, String pn, Module other) {
                return m.isReflectivelyExported(pn, other);
            }
            public boolean isReflectivelyOpened(Module m, String pn, Module other) {
                return m.isReflectivelyOpened(pn, other);
            }
            public Module addEnableNativeAccess(Module m) {
                return m.implAddEnableNativeAccess();
            }
            public boolean addEnableNativeAccess(ModuleLayer layer, String name) {
                return layer.addEnableNativeAccess(name);
            }
            public void addEnableNativeAccessToAllUnnamed() {
                Module.implAddEnableNativeAccessToAllUnnamed();
            }
            public void ensureNativeAccess(Module m, Class<?> owner, String methodName, Class<?> currentClass, boolean jni) {
                m.ensureNativeAccess(owner, methodName, currentClass, jni);
            }
            public ServicesCatalog getServicesCatalog(ModuleLayer layer) {
                return layer.getServicesCatalog();
            }
            public void bindToLoader(ModuleLayer layer, ClassLoader loader) {
                layer.bindToLoader(loader);
            }
            public Stream<ModuleLayer> layers(ModuleLayer layer) {
                return layer.layers();
            }
            public Stream<ModuleLayer> layers(ClassLoader loader) {
                return ModuleLayer.layers(loader);
            }

            public int uncheckedCountPositives(byte[] bytes, int offset, int length) {
                return StringCoding.countPositives(bytes, offset, length);
            }
            public int countNonZeroAscii(String s) {
                return StringCoding.countNonZeroAscii(s);
            }
            public String uncheckedNewStringNoRepl(byte[] bytes, Charset cs) throws CharacterCodingException  {
                return String.newStringNoRepl(bytes, cs);
            }
            public char uncheckedGetUTF16Char(byte[] bytes, int index) {
                return StringUTF16.getChar(bytes, index);
            }
            public void uncheckedPutCharUTF16(byte[] bytes, int index, int ch) {
                StringUTF16.putChar(bytes, index, ch);
            }
            public byte[] uncheckedGetBytesNoRepl(String s, Charset cs) throws CharacterCodingException {
                return String.getBytesNoRepl(s, cs);
            }

            public String newStringUTF8NoRepl(byte[] bytes, int off, int len) {
                return String.newStringUTF8NoRepl(bytes, off, len, true);
            }

            public byte[] getBytesUTF8NoRepl(String s) {
                return String.getBytesUTF8NoRepl(s);
            }

            public void uncheckedInflateBytesToChars(byte[] src, int srcOff, char[] dst, int dstOff, int len) {
                StringLatin1.inflate(src, srcOff, dst, dstOff, len);
            }

            public int uncheckedDecodeASCII(byte[] src, int srcOff, char[] dst, int dstOff, int len) {
                return String.decodeASCII(src, srcOff, dst, dstOff, len);
            }

            public int uncheckedEncodeASCII(char[] src, int srcOff, byte[] dst, int dstOff, int len) {
                return StringCoding.implEncodeAsciiArray(src, srcOff, dst, dstOff, len);
            }

            public InputStream initialSystemIn() {
                return initialIn;
            }

            public PrintStream initialSystemErr() {
                return initialErr;
            }

            public void setCause(Throwable t, Throwable cause) {
                t.setCause(cause);
            }

            public ProtectionDomain protectionDomain(Class<?> c) {
                return c.getProtectionDomain();
            }

            public MethodHandle stringConcatHelper(String name, MethodType methodType) {
                return StringConcatHelper.lookupStatic(name, methodType);
            }

            public long stringConcatInitialCoder() {
                return StringConcatHelper.initialCoder();
            }

            public long stringConcatMix(long lengthCoder, String constant) {
                return StringConcatHelper.mix(lengthCoder, constant);
            }

            public long stringConcatMix(long lengthCoder, char value) {
                return StringConcatHelper.mix(lengthCoder, value);
            }

            public Object uncheckedStringConcat1(String[] constants) {
                return new StringConcatHelper.Concat1(constants);
            }

            public byte stringInitCoder() {
                return String.COMPACT_STRINGS ? String.LATIN1 : String.UTF16;
            }

            public byte stringCoder(String str) {
                return str.coder();
            }

            public String join(String prefix, String suffix, String delimiter, String[] elements, int size) {
                return String.join(prefix, suffix, delimiter, elements, size);
            }

            public String concat(String prefix, Object value, String suffix) {
                return StringConcatHelper.concat(prefix, value, suffix);
            }

            public Object classData(Class<?> c) {
                return c.getClassData();
            }

            @Override
            public NativeLibraries nativeLibrariesFor(ClassLoader loader) {
                return ClassLoader.nativeLibrariesFor(loader);
            }

            public Thread[] getAllThreads() {
                return Thread.getAllThreads();
            }

            public ThreadContainer threadContainer(Thread thread) {
                return thread.threadContainer();
            }

            public void start(Thread thread, ThreadContainer container) {
                thread.start(container);
            }

            public StackableScope headStackableScope(Thread thread) {
                return thread.headStackableScopes();
            }

            public void setHeadStackableScope(StackableScope scope) {
                Thread.setHeadStackableScope(scope);
            }

            public Thread currentCarrierThread() {
                return Thread.currentCarrierThread();
            }

            public <T> T getCarrierThreadLocal(CarrierThreadLocal<T> local) {
                return ((ThreadLocal<T>)local).getCarrierThreadLocal();
            }

            public <T> void setCarrierThreadLocal(CarrierThreadLocal<T> local, T value) {
                ((ThreadLocal<T>)local).setCarrierThreadLocal(value);
            }

            public void removeCarrierThreadLocal(CarrierThreadLocal<?> local) {
                ((ThreadLocal<?>)local).removeCarrierThreadLocal();
            }

            public Object[] scopedValueCache() {
                return Thread.scopedValueCache();
            }

            public void setScopedValueCache(Object[] cache) {
                Thread.setScopedValueCache(cache);
            }

            public Object scopedValueBindings() {
                return Thread.scopedValueBindings();
            }

            public Continuation getContinuation(Thread thread) {
                return thread.getContinuation();
            }

            public void setContinuation(Thread thread, Continuation continuation) {
                thread.setContinuation(continuation);
            }

            public ContinuationScope virtualThreadContinuationScope() {
                return VirtualThread.continuationScope();
            }

            public void parkVirtualThread() {
                Thread thread = Thread.currentThread();
                if (thread instanceof BaseVirtualThread vthread) {
                    vthread.park();
                } else {
                    throw new WrongThreadException();
                }
            }

            public void parkVirtualThread(long nanos) {
                Thread thread = Thread.currentThread();
                if (thread instanceof BaseVirtualThread vthread) {
                    vthread.parkNanos(nanos);
                } else {
                    throw new WrongThreadException();
                }
            }

            public void unparkVirtualThread(Thread thread) {
                if (thread instanceof BaseVirtualThread vthread) {
                    vthread.unpark();
                } else {
                    throw new WrongThreadException();
                }
            }

            public Executor virtualThreadDefaultScheduler() {
                return VirtualThread.defaultScheduler();
            }

            public StackWalker newStackWalkerInstance(Set<StackWalker.Option> options,
                                                      ContinuationScope contScope,
                                                      Continuation continuation) {
                return StackWalker.newInstance(options, null, contScope, continuation);
            }

            public String getLoaderNameID(ClassLoader loader) {
                return loader != null ? loader.nameAndId() : "null";
            }

            @Override
            public void copyToSegmentRaw(String string, MemorySegment segment, long offset) {
                string.copyToSegmentRaw(segment, offset);
            }

            @Override
            public boolean bytesCompatible(String string, Charset charset) {
                return string.bytesCompatible(charset);
            }
        });
    }
}
