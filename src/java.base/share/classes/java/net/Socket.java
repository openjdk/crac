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

package java.net;

import jdk.internal.access.SharedSecrets;
import jdk.internal.event.SocketReadEvent;
import jdk.internal.event.SocketWriteEvent;
import jdk.internal.invoke.MhUtil;

import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.channels.ClosedByInterruptException;
import java.nio.channels.SocketChannel;
import java.util.Objects;
import java.util.Set;
import java.util.Collections;

/**
 * This class implements client sockets (also called just
 * "sockets"). A socket is an endpoint for communication
 * between two machines.
 * <p>
 * The actual work of the socket is performed by an instance of the
 * {@code SocketImpl} class.
 *
 * <p> The {@code Socket} class defines convenience
 * methods to set and get several socket options. This class also
 * defines the {@link #setOption(SocketOption, Object) setOption}
 * and {@link #getOption(SocketOption) getOption} methods to set
 * and query socket options.
 * A {@code Socket} support the following options:
 * <blockquote>
 * <table class="striped">
 * <caption style="display:none">Socket options</caption>
 * <thead>
 *   <tr>
 *     <th scope="col">Option Name</th>
 *     <th scope="col">Description</th>
 *   </tr>
 * </thead>
 * <tbody>
 *   <tr>
 *     <th scope="row"> {@link java.net.StandardSocketOptions#SO_SNDBUF SO_SNDBUF} </th>
 *     <td> The size of the socket send buffer </td>
 *   </tr>
 *   <tr>
 *     <th scope="row"> {@link java.net.StandardSocketOptions#SO_RCVBUF SO_RCVBUF} </th>
 *     <td> The size of the socket receive buffer </td>
 *   </tr>
 *   <tr>
 *     <th scope="row"> {@link java.net.StandardSocketOptions#SO_KEEPALIVE SO_KEEPALIVE} </th>
 *     <td> Keep connection alive </td>
 *   </tr>
 *   <tr>
 *     <th scope="row"> {@link java.net.StandardSocketOptions#SO_REUSEADDR SO_REUSEADDR} </th>
 *     <td> Re-use address </td>
 *   </tr>
 *   <tr>
 *     <th scope="row"> {@link java.net.StandardSocketOptions#SO_LINGER SO_LINGER} </th>
 *     <td> Linger on close if data is present (when configured in blocking mode
 *          only) </td>
 *   </tr>
 *   <tr>
 *     <th scope="row"> {@link java.net.StandardSocketOptions#TCP_NODELAY TCP_NODELAY} </th>
 *     <td> Disable the Nagle algorithm </td>
 *   </tr>
 * </tbody>
 * </table>
 * </blockquote>
 * Additional (implementation specific) options may also be supported.
 *
 * @see     java.net.SocketImpl
 * @see     java.nio.channels.SocketChannel
 * @since   1.0
 */
public class Socket implements java.io.Closeable {
    private static final VarHandle STATE, IN, OUT;
    static {
        MethodHandles.Lookup l = MethodHandles.lookup();
        STATE = MhUtil.findVarHandle(l, "state", int.class);
        IN = MhUtil.findVarHandle(l, "in", InputStream.class);
        OUT = MhUtil.findVarHandle(l, "out", OutputStream.class);
    }

    // the underlying SocketImpl, may be null, may be swapped when connecting
    private volatile SocketImpl impl;

    // state bits
    private static final int SOCKET_CREATED = 1 << 0;   // impl.create(boolean) called
    private static final int BOUND          = 1 << 1;
    private static final int CONNECTED      = 1 << 2;
    private static final int CLOSED         = 1 << 3;
    private static final int SHUT_IN        = 1 << 9;
    private static final int SHUT_OUT       = 1 << 10;
    private volatile int state;

    // used to coordinate creating and closing underlying socket
    private final Object socketLock = new Object();

    // input/output streams
    private volatile InputStream in;
    private volatile OutputStream out;

    /**
     * Atomically sets state to the result of a bitwise OR of the current value
     * and the given mask.
     * @return the previous state value
     */
    private int getAndBitwiseOrState(int mask) {
        return (int) STATE.getAndBitwiseOr(this, mask);
    }

    private static boolean isBound(int s) {
        return (s & BOUND) != 0;
    }

    private static boolean isConnected(int s) {
        return (s & CONNECTED) != 0;
    }

    private static boolean isClosed(int s) {
        return (s & CLOSED) != 0;
    }

    private static boolean isInputShutdown(int s) {
        return (s & SHUT_IN) != 0;
    }

    private static boolean isOutputShutdown(int s) {
        return (s & SHUT_OUT) != 0;
    }

    /**
     * Creates an unconnected Socket.
     * <p>
     * If the application has specified a {@linkplain SocketImplFactory client
     * socket implementation factory}, that factory's
     * {@linkplain SocketImplFactory#createSocketImpl() createSocketImpl}
     * method is called to create the actual socket implementation. Otherwise
     * a system-default socket implementation is created.
     *
     * @since   1.1
     */
    public Socket() {
        this.impl = createImpl();
    }

    /**
     * Creates an unconnected socket, specifying the type of proxy, if any,
     * that should be used regardless of any other settings.
     * <P>
     * Examples:
     * <UL> <LI>{@code Socket s = new Socket(Proxy.NO_PROXY);} will create
     * a plain socket ignoring any other proxy configuration.</LI>
     * <LI>{@code Socket s = new Socket(new Proxy(Proxy.Type.SOCKS, new InetSocketAddress("socks.mydom.com", 1080)));}
     * will create a socket connecting through the specified SOCKS proxy
     * server.</LI>
     * </UL>
     *
     * @param proxy a {@link java.net.Proxy Proxy} object specifying what kind
     *              of proxying should be used.
     * @throws IllegalArgumentException if the proxy is of an invalid type
     *          or {@code null}.
     * @see java.net.ProxySelector
     * @see java.net.Proxy
     *
     * @since   1.5
     */
    @SuppressWarnings("this-escape")
    public Socket(Proxy proxy) {
        // Create a copy of Proxy as a security measure
        if (proxy == null) {
            throw new IllegalArgumentException("Invalid Proxy");
        }
        Proxy p = proxy == Proxy.NO_PROXY ? Proxy.NO_PROXY
                                          : sun.net.ApplicationProxy.create(proxy);
        Proxy.Type type = p.type();
        if (type == Proxy.Type.SOCKS || type == Proxy.Type.HTTP) {
            InetSocketAddress epoint = (InetSocketAddress) p.address();
            if (epoint.getAddress() != null) {
                checkAddress (epoint.getAddress(), "Socket");
            }
            // create a SOCKS or HTTP SocketImpl that delegates to a platform SocketImpl
            SocketImpl delegate = SocketImpl.createPlatformSocketImpl(false);
            impl = (type == Proxy.Type.SOCKS) ? new SocksSocketImpl(p, delegate)
                                              : new HttpConnectSocketImpl(p, delegate, this);
        } else {
            if (p == Proxy.NO_PROXY) {
                // create a platform or custom SocketImpl for the DIRECT case
                SocketImplFactory factory = Socket.factory;
                if (factory == null) {
                    impl = SocketImpl.createPlatformSocketImpl(false);
                } else {
                    impl = factory.createSocketImpl();
                }
            } else
                throw new IllegalArgumentException("Invalid Proxy");
        }
    }

    /**
     * Creates an unconnected Socket with a user-specified
     * SocketImpl.
     *
     * @param impl an instance of a <B>SocketImpl</B>
     * the subclass wishes to use on the Socket.
     *
     * @throws    SocketException if there is an error in the underlying protocol,
     * such as a TCP error.
     *
     * @since   1.1
     */
    protected Socket(SocketImpl impl) throws SocketException {
        if (impl != null) {
            this.impl = impl;
        }
    }

    /**
     * Creates a stream socket and connects it to the specified port
     * number on the named host.
     * <p>
     * If the specified host is {@code null} it is the equivalent of
     * specifying the address as
     * {@link java.net.InetAddress#getByName InetAddress.getByName}{@code (null)}.
     * In other words, it is equivalent to specifying an address of the
     * loopback interface. </p>
     * <p>
     * If the application has specified a {@linkplain SocketImplFactory client
     * socket implementation factory}, that factory's
     * {@linkplain SocketImplFactory#createSocketImpl() createSocketImpl}
     * method is called to create the actual socket implementation. Otherwise
     * a system-default socket implementation is created.
     *
     * @param      host   the host name, or {@code null} for the loopback address.
     * @param      port   the port number.
     *
     * @throws     UnknownHostException if the IP address of
     * the host could not be determined.
     *
     * @throws     IOException  if an I/O error occurs when creating the socket.
     * @throws     IllegalArgumentException if the port parameter is outside
     *             the specified range of valid port values, which is between
     *             0 and 65535, inclusive.
     */
    @SuppressWarnings("this-escape")
    public Socket(String host, int port)
        throws UnknownHostException, IOException
    {
        this(host != null ? new InetSocketAddress(host, port) :
             new InetSocketAddress(InetAddress.getByName(null), port),
             (SocketAddress) null, true);
    }

    /**
     * Creates a stream socket and connects it to the specified port
     * number at the specified IP address.
     * <p>
     * If the application has specified a {@linkplain SocketImplFactory client
     * socket implementation factory}, that factory's
     * {@linkplain SocketImplFactory#createSocketImpl() createSocketImpl}
     * method is called to create the actual socket implementation. Otherwise
     * a system-default socket implementation is created.
     *
     * @param      address   the IP address.
     * @param      port      the port number.
     * @throws     IOException  if an I/O error occurs when creating the socket.
     * @throws     IllegalArgumentException if the port parameter is outside
     *             the specified range of valid port values, which is between
     *             0 and 65535, inclusive.
     * @throws     NullPointerException if {@code address} is null.
     */
    @SuppressWarnings("this-escape")
    public Socket(InetAddress address, int port) throws IOException {
        this(address != null ? new InetSocketAddress(address, port) : null,
             (SocketAddress) null, true);
    }

    /**
     * Creates a socket and connects it to the specified remote host on
     * the specified remote port. The Socket will also bind() to the local
     * address and port supplied.
     * <p>
     * If the specified host is {@code null} it is the equivalent of
     * specifying the address as
     * {@link java.net.InetAddress#getByName InetAddress.getByName}{@code (null)}.
     * In other words, it is equivalent to specifying an address of the
     * loopback interface. </p>
     * <p>
     * A local port number of {@code zero} will let the system pick up a
     * free port in the {@code bind} operation.</p>
     *
     * @param host the name of the remote host, or {@code null} for the loopback address.
     * @param port the remote port
     * @param localAddr the local address the socket is bound to, or
     *        {@code null} for the {@code anyLocal} address.
     * @param localPort the local port the socket is bound to, or
     *        {@code zero} for a system selected free port.
     * @throws     IOException  if an I/O error occurs when creating the socket.
     * @throws     IllegalArgumentException if the port parameter or localPort
     *             parameter is outside the specified range of valid port values,
     *             which is between 0 and 65535, inclusive.
     * @since   1.1
     */
    @SuppressWarnings("this-escape")
    public Socket(String host, int port, InetAddress localAddr,
                  int localPort) throws IOException {
        this(host != null ? new InetSocketAddress(host, port) :
               new InetSocketAddress(InetAddress.getByName(null), port),
             new InetSocketAddress(localAddr, localPort), true);
    }

    /**
     * Creates a socket and connects it to the specified remote address on
     * the specified remote port. The Socket will also bind() to the local
     * address and port supplied.
     * <p>
     * If the specified local address is {@code null} it is the equivalent of
     * specifying the address as the AnyLocal address
     * (see {@link java.net.InetAddress#isAnyLocalAddress InetAddress.isAnyLocalAddress}{@code ()}).
     * <p>
     * A local port number of {@code zero} will let the system pick up a
     * free port in the {@code bind} operation.</p>
     *
     * @param address the remote address
     * @param port the remote port
     * @param localAddr the local address the socket is bound to, or
     *        {@code null} for the {@code anyLocal} address.
     * @param localPort the local port the socket is bound to or
     *        {@code zero} for a system selected free port.
     * @throws     IOException  if an I/O error occurs when creating the socket.
     * @throws     IllegalArgumentException if the port parameter or localPort
     *             parameter is outside the specified range of valid port values,
     *             which is between 0 and 65535, inclusive.
     * @throws     NullPointerException if {@code address} is null.
     * @since   1.1
     */
    @SuppressWarnings("this-escape")
    public Socket(InetAddress address, int port, InetAddress localAddr,
                  int localPort) throws IOException {
        this(address != null ? new InetSocketAddress(address, port) : null,
             new InetSocketAddress(localAddr, localPort), true);
    }

    /**
     * Creates a stream socket and connects it to the specified port
     * number on the named host.
     * <p>
     * If the specified host is {@code null} it is the equivalent of
     * specifying the address as
     * {@link java.net.InetAddress#getByName InetAddress.getByName}{@code (null)}.
     * In other words, it is equivalent to specifying an address of the
     * loopback interface. </p>
     * <p>
     * If the application has specified a {@linkplain SocketImplFactory client
     * socket implementation factory}, that factory's
     * {@linkplain SocketImplFactory#createSocketImpl() createSocketImpl}
     * method is called to create the actual socket implementation. Otherwise
     * a system-default socket implementation is created.
     *
     * @param      host     the host name, or {@code null} for the loopback address.
     * @param      port     the port number.
     * @param      stream   must be true, false is not allowed.
     * @throws     IOException  if an I/O error occurs when creating the socket.
     * @throws     IllegalArgumentException if the stream parameter is {@code false}
     *             or if the port parameter is outside the specified range of valid
     *             port values, which is between 0 and 65535, inclusive.
     * @deprecated The {@code stream} parameter provided a way in early JDK releases
     *             to create a {@code Socket} that used a datagram socket. This feature
     *             no longer exists. Instead use {@link DatagramSocket} for datagram sockets.
     */
    @Deprecated(forRemoval = true, since = "1.1")
    @SuppressWarnings("this-escape")
    public Socket(String host, int port, boolean stream) throws IOException {
        this(host != null ? new InetSocketAddress(host, port) :
               new InetSocketAddress(InetAddress.getByName(null), port),
             (SocketAddress) null, stream);
    }

    /**
     * Creates a socket and connects it to the specified port number at
     * the specified IP address.
     * <p>
     * If the application has specified a {@linkplain SocketImplFactory client
     * socket implementation factory}, that factory's
     * {@linkplain SocketImplFactory#createSocketImpl() createSocketImpl}
     * method is called to create the actual socket implementation. Otherwise
     * a system-default socket implementation is created.
     *
     * @param      host     the IP address.
     * @param      port      the port number.
     * @param      stream    must be true, false is not allowed.
     * @throws     IOException  if an I/O error occurs when creating the socket.
     * @throws     IllegalArgumentException if the stream parameter is {@code false}
     *             or if the port parameter is outside the specified range of valid
     *             port values, which is between 0 and 65535, inclusive.
     * @throws     NullPointerException if {@code host} is null.
     * @deprecated The {@code stream} parameter provided a way in early JDK releases
     *             to create a {@code Socket} that used a datagram socket. This feature
     *             no longer exists. Instead use {@link DatagramSocket} for datagram sockets.
     */
    @Deprecated(forRemoval = true, since = "1.1")
    @SuppressWarnings("this-escape")
    public Socket(InetAddress host, int port, boolean stream) throws IOException {
        this(host != null ? new InetSocketAddress(host, port) : null,
             new InetSocketAddress(0), stream);
    }

    /**
     * Initialize a new Socket that is connected to the given remote address.
     * The socket is optionally bound to a local address before connecting.
     *
     * @param address the remote address to connect to
     * @param localAddr the local address to bind to, can be null
     * @param stream true for a stream socket, false for a datagram socket
     */
    private Socket(SocketAddress address, SocketAddress localAddr, boolean stream)
        throws IOException
    {
        Objects.requireNonNull(address);
        if (!stream) {
            throw new IllegalArgumentException(
                    "Socket constructor does not support creation of datagram sockets");
        }
        assert address instanceof InetSocketAddress;

        // create the SocketImpl and the underlying socket
        SocketImpl impl = createImpl();
        impl.create(stream);

        this.impl = impl;
        this.state = SOCKET_CREATED;

        try {
            if (localAddr != null) {
                bind(localAddr);
            }
            connect(address);
        } catch (Throwable throwable) {
            closeSuppressingExceptions(throwable);
            throw throwable;
        }
    }

    /**
     * Create a new SocketImpl for a connecting/client socket. The SocketImpl
     * is created without an underlying socket.
     */
    private static SocketImpl createImpl() {
        SocketImplFactory factory = Socket.factory;
        if (factory != null) {
            return factory.createSocketImpl();
        } else {
            // create a SOCKS SocketImpl that delegates to a platform SocketImpl
            SocketImpl delegate = SocketImpl.createPlatformSocketImpl(false);
            return new SocksSocketImpl(delegate);
        }
    }

    /**
     * Returns the {@code SocketImpl} for this Socket, creating it, and the
     * underlying socket, if required.
     * @throws SocketException if creating the underlying socket fails
     */
    private SocketImpl getImpl() throws SocketException {
        if ((state & SOCKET_CREATED) == 0) {
            synchronized (socketLock) {
                int s = state;   // re-read state
                if ((s & SOCKET_CREATED) == 0) {
                    if (isClosed(s)) {
                        throw new SocketException("Socket is closed");
                    }
                    SocketImpl impl = this.impl;
                    if (impl == null) {
                        this.impl = impl = createImpl();
                    }
                    try {
                        impl.create(true);
                    } catch (SocketException e) {
                        throw e;
                    } catch (IOException e) {
                        throw new SocketException(e.getMessage(), e);
                    }
                    getAndBitwiseOrState(SOCKET_CREATED);
                }
            }
        }
        return impl;
    }

    /**
     * Returns the SocketImpl, may be null.
     */
    SocketImpl impl() {
        return impl;
    }

    /**
     * Sets the SocketImpl. The SocketImpl is connected to a peer. The behavior for
     * the case that the Socket was not a newly created Socket is unspecified. If
     * there is an existing SocketImpl then it closed to avoid leaking resources.
     * @throws SocketException if the Socket is closed
     * @apiNote For ServerSocket use when accepting connections
     */
    void setConnectedImpl(SocketImpl si) throws SocketException {
        synchronized (socketLock) {
            if ((state & CLOSED) != 0) {
                throw new SocketException("Socket is closed");
            }
            SocketImpl previous = impl;
            impl = si;
            state = (SOCKET_CREATED | BOUND | CONNECTED);
            if (previous != null) {
                in = null;
                out = null;
                previous.closeQuietly();
            }
        }
    }

    /**
     * Sets the SocketImpl.
     * @apiNote For ServerSocket use when accepting connections with a custom SocketImpl
     */
    void setImpl(SocketImpl si) {
        impl = si;
    }

    /**
     * Sets to Socket state for a newly accepted connection.
     * @apiNote For ServerSocket use when accepting connections with a custom SocketImpl
     */
    void setConnected() {
        getAndBitwiseOrState(SOCKET_CREATED | BOUND | CONNECTED);
    }

    /**
     * Connects this socket to the server.
     *
     * <p> If the connection cannot be established, then the socket is closed,
     * and an {@link IOException} is thrown.
     *
     * <p> This method is {@linkplain Thread#interrupt() interruptible} in the
     * following circumstances:
     * <ol>
     *   <li> The socket is {@linkplain SocketChannel#socket() associated} with
     *        a {@link SocketChannel SocketChannel}.
     *        In that case, interrupting a thread establishing a connection will
     *        close the underlying channel and cause this method to throw
     *        {@link ClosedByInterruptException} with the interrupt status set.
     *   <li> The socket uses the system-default socket implementation and a
     *        {@linkplain Thread#isVirtual() virtual thread} is establishing a
     *        connection. In that case, interrupting the virtual thread will
     *        cause it to wakeup and close the socket. This method will then throw
     *        {@code SocketException} with the interrupt status set.
     * </ol>
     *
     * @param   endpoint the {@code SocketAddress}
     * @throws  IOException if an error occurs during the connection, the socket
     *          is already connected or the socket is closed
     * @throws  UnknownHostException if the connection could not be established
     *          because the endpoint is an unresolved {@link InetSocketAddress}
     * @throws  java.nio.channels.IllegalBlockingModeException
     *          if this socket has an associated channel,
     *          and the channel is in non-blocking mode
     * @throws  IllegalArgumentException if endpoint is null or is a
     *          SocketAddress subclass not supported by this socket
     * @since 1.4
     */
    public void connect(SocketAddress endpoint) throws IOException {
        connect(endpoint, 0);
    }

    /**
     * Connects this socket to the server with a specified timeout value.
     * A timeout of zero is interpreted as an infinite timeout. The connection
     * will then block until established or an error occurs.
     *
     * <p> If the connection cannot be established, or the timeout expires
     * before the connection is established, then the socket is closed, and an
     * {@link IOException} is thrown.
     *
     * <p> This method is {@linkplain Thread#interrupt() interruptible} in the
     * following circumstances:
     * <ol>
     *   <li> The socket is {@linkplain SocketChannel#socket() associated} with
     *        a {@link SocketChannel SocketChannel}.
     *        In that case, interrupting a thread establishing a connection will
     *        close the underlying channel and cause this method to throw
     *        {@link ClosedByInterruptException} with the interrupt status set.
     *   <li> The socket uses the system-default socket implementation and a
     *        {@linkplain Thread#isVirtual() virtual thread} is establishing a
     *        connection. In that case, interrupting the virtual thread will
     *        cause it to wakeup and close the socket. This method will then throw
     *        {@code SocketException} with the interrupt status set.
     * </ol>
     *
     * @param   endpoint the {@code SocketAddress}
     * @param   timeout  the timeout value to be used in milliseconds.
     * @throws  IOException if an error occurs during the connection, the socket
     *          is already connected or the socket is closed
     * @throws  SocketTimeoutException if timeout expires before connecting
     * @throws  UnknownHostException if the connection could not be established
     *          because the endpoint is an unresolved {@link InetSocketAddress}
     * @throws  java.nio.channels.IllegalBlockingModeException
     *          if this socket has an associated channel,
     *          and the channel is in non-blocking mode
     * @throws  IllegalArgumentException if endpoint is null or is a
     *          SocketAddress subclass not supported by this socket, or
     *          if {@code timeout} is negative
     * @since 1.4
     */
    public void connect(SocketAddress endpoint, int timeout) throws IOException {
        if (endpoint == null)
            throw new IllegalArgumentException("connect: The address can't be null");

        if (timeout < 0)
            throw new IllegalArgumentException("connect: timeout can't be negative");

        int s = state;
        if (isClosed(s))
            throw new SocketException("Socket is closed");
        if (isConnected(s))
            throw new SocketException("Already connected");

        if (!(endpoint instanceof InetSocketAddress epoint))
            throw new IllegalArgumentException("Unsupported address type");

        InetAddress addr = epoint.getAddress();
        checkAddress(addr, "connect");

        try {
            getImpl().connect(epoint, timeout);
        } catch (IOException error) {
            closeSuppressingExceptions(error);
            throw error;
        }

        // connect will bind the socket if not previously bound
        getAndBitwiseOrState(BOUND | CONNECTED);
    }

    /**
     * Binds the socket to a local address.
     * <P>
     * If the address is {@code null}, then the system will pick up
     * an ephemeral port and a valid local address to bind the socket.
     *
     * @param   bindpoint the {@code SocketAddress} to bind to
     * @throws  IOException if the bind operation fails, the socket
     *          is already bound or the socket is closed.
     * @throws  IllegalArgumentException if bindpoint is a
     *          SocketAddress subclass not supported by this socket
     *
     * @since   1.4
     * @see #isBound()
     */
    public void bind(SocketAddress bindpoint) throws IOException {
        int s = state;
        if (isClosed(s))
            throw new SocketException("Socket is closed");
        if (isBound(s))
            throw new SocketException("Already bound");

        if (bindpoint != null && (!(bindpoint instanceof InetSocketAddress)))
            throw new IllegalArgumentException("Unsupported address type");
        InetSocketAddress epoint = (InetSocketAddress) bindpoint;
        if (epoint != null && epoint.isUnresolved())
            throw new SocketException("Unresolved address");
        if (epoint == null) {
            epoint = new InetSocketAddress(0);
        }
        InetAddress addr = epoint.getAddress();
        int port = epoint.getPort();
        checkAddress (addr, "bind");
        getImpl().bind(addr, port);
        getAndBitwiseOrState(BOUND);
    }

    private void checkAddress(InetAddress addr, String op) {
        if (addr == null) {
            return;
        }
        if (!(addr instanceof Inet4Address || addr instanceof Inet6Address)) {
            throw new IllegalArgumentException(op + ": invalid address type");
        }
    }

    /**
     * Returns the address to which the socket is connected.
     * <p>
     * If the socket was connected prior to being {@link #close closed},
     * then this method will continue to return the connected address
     * after the socket is closed.
     *
     * @return  the remote IP address to which this socket is connected,
     *          or {@code null} if the socket is not connected.
     */
    public InetAddress getInetAddress() {
        if (!isConnected())
            return null;
        try {
            return getImpl().getInetAddress();
        } catch (SocketException e) {
        }
        return null;
    }

    /**
     * Gets the local address to which the socket is bound.
     *
     * @return the local address to which the socket is bound, or
     *         the wildcard address if the socket is closed or not bound yet.
     * @since   1.1
     */
    public InetAddress getLocalAddress() {
        // This is for backward compatibility
        if (!isBound())
            return InetAddress.anyLocalAddress();
        InetAddress in = null;
        try {
            in = (InetAddress) getImpl().getOption(SocketOptions.SO_BINDADDR);
            if (in.isAnyLocalAddress()) {
                in = InetAddress.anyLocalAddress();
            }
        } catch (Exception e) {
            in = InetAddress.anyLocalAddress(); // "0.0.0.0"
        }
        return in;
    }

    /**
     * Returns the remote port number to which this socket is connected.
     * <p>
     * If the socket was connected prior to being {@link #close closed},
     * then this method will continue to return the connected port number
     * after the socket is closed.
     *
     * @return  the remote port number to which this socket is connected, or
     *          0 if the socket is not connected yet.
     */
    public int getPort() {
        if (!isConnected())
            return 0;
        try {
            return getImpl().getPort();
        } catch (SocketException e) {
            // Shouldn't happen as we're connected
        }
        return -1;
    }

    /**
     * Returns the local port number to which this socket is bound.
     * <p>
     * If the socket was bound prior to being {@link #close closed},
     * then this method will continue to return the local port number
     * after the socket is closed.
     *
     * @return  the local port number to which this socket is bound or -1
     *          if the socket is not bound yet.
     */
    public int getLocalPort() {
        if (!isBound())
            return -1;
        try {
            return getImpl().getLocalPort();
        } catch(SocketException e) {
            // shouldn't happen as we're bound
        }
        return -1;
    }

    /**
     * Returns the address of the endpoint this socket is connected to, or
     * {@code null} if it is unconnected.
     * <p>
     * If the socket was connected prior to being {@link #close closed},
     * then this method will continue to return the connected address
     * after the socket is closed.
     *
     * @return a {@code SocketAddress} representing the remote endpoint of this
     *         socket, or {@code null} if it is not connected yet.
     * @see #getInetAddress()
     * @see #getPort()
     * @see #connect(SocketAddress, int)
     * @see #connect(SocketAddress)
     * @since 1.4
     */
    public SocketAddress getRemoteSocketAddress() {
        if (!isConnected())
            return null;
        return new InetSocketAddress(getInetAddress(), getPort());
    }

    /**
     * Returns the address of the endpoint this socket is bound to.
     * <p>
     * If a socket bound to an endpoint represented by an
     * {@code InetSocketAddress } is {@link #close closed},
     * then this method will continue to return an {@code InetSocketAddress}
     * after the socket is closed. In that case the returned
     * {@code InetSocketAddress}'s address is the
     * {@link InetAddress#isAnyLocalAddress wildcard} address
     * and its port is the local port that it was bound to.
     *
     * @return a {@code SocketAddress} representing the local endpoint of
     *         this socket, or {@code null} if the socket is not bound yet.
     *
     * @see #getLocalAddress()
     * @see #getLocalPort()
     * @see #bind(SocketAddress)
     * @since 1.4
     */
    public SocketAddress getLocalSocketAddress() {
        if (!isBound())
            return null;
        return new InetSocketAddress(getLocalAddress(), getLocalPort());
    }

    /**
     * Returns the unique {@link java.nio.channels.SocketChannel SocketChannel}
     * object associated with this socket, if any.
     *
     * <p> A socket will have a channel if, and only if, the channel itself was
     * created via the {@link java.nio.channels.SocketChannel#open
     * SocketChannel.open} or {@link
     * java.nio.channels.ServerSocketChannel#accept ServerSocketChannel.accept}
     * methods.
     *
     * @return  the socket channel associated with this socket,
     *          or {@code null} if this socket was not created
     *          for a channel
     *
     * @since 1.4
     */
    public SocketChannel getChannel() {
        return null;
    }

    /**
     * Returns an input stream for this socket.
     *
     * <p> If this socket has an associated channel then the resulting input
     * stream delegates all of its operations to the channel.  If the channel
     * is in non-blocking mode then the input stream's {@code read} operations
     * will throw an {@link java.nio.channels.IllegalBlockingModeException}.
     *
     * <p> Reading from the input stream is {@linkplain Thread#interrupt()
     * interruptible} in the following circumstances:
     * <ol>
     *   <li> The socket is {@linkplain SocketChannel#socket() associated} with
     *        a {@link SocketChannel SocketChannel}.
     *        In that case, interrupting a thread reading from the input stream
     *        will close the underlying channel and cause the read method to
     *        throw {@link ClosedByInterruptException} with the interrupt
     *        status set.
     *   <li> The socket uses the system-default socket implementation and a
     *        {@linkplain Thread#isVirtual() virtual thread} is reading from the
     *        input stream. In that case, interrupting the virtual thread will
     *        cause it to wakeup and close the socket. The read method will then
     *        throw {@code SocketException} with the interrupt status set.
     * </ol>
     *
     * <p>Under abnormal conditions the underlying connection may be
     * broken by the remote host or the network software (for example
     * a connection reset in the case of TCP connections). When a
     * broken connection is detected by the network software the
     * following applies to the returned input stream :-
     *
     * <ul>
     *
     *   <li><p>The network software may discard bytes that are buffered
     *   by the socket. Bytes that aren't discarded by the network
     *   software can be read using {@link java.io.InputStream#read read}.
     *
     *   <li><p>If there are no bytes buffered on the socket, or all
     *   buffered bytes have been consumed by
     *   {@link java.io.InputStream#read read}, then all subsequent
     *   calls to {@link java.io.InputStream#read read} will throw an
     *   {@link java.io.IOException IOException}.
     *
     *   <li><p>If there are no bytes buffered on the socket, and the
     *   socket has not been closed using {@link #close close}, then
     *   {@link java.io.InputStream#available available} will
     *   return {@code 0}.
     *
     * </ul>
     *
     * <p> Closing the returned {@link java.io.InputStream InputStream}
     * will close the associated socket.
     *
     * @return     an input stream for reading bytes from this socket.
     * @throws     IOException  if an I/O error occurs when creating the
     *             input stream, the socket is closed, the socket is
     *             not connected, or the socket input has been shutdown
     *             using {@link #shutdownInput()}
     */
    public InputStream getInputStream() throws IOException {
        int s = state;
        if (isClosed(s))
            throw new SocketException("Socket is closed");
        if (!isConnected(s))
            throw new SocketException("Socket is not connected");
        if (isInputShutdown(s))
            throw new SocketException("Socket input is shutdown");
        InputStream in = this.in;
        if (in == null) {
            // wrap the input stream so that the close method closes this socket
            in = new SocketInputStream(this, impl.getInputStream());
            if (!IN.compareAndSet(this, null, in)) {
                in = this.in;
            }
        }
        return in;
    }

    /**
     * An InputStream that delegates read/available operations to an underlying
     * input stream. The close method is overridden to close the Socket.
     */
    private static class SocketInputStream extends InputStream {
        private final Socket parent;
        private final InputStream in;
        SocketInputStream(Socket parent, InputStream in) {
            this.parent = parent;
            this.in = in;
        }
        @Override
        public int read() throws IOException {
            byte[] a = new byte[1];
            int n = read(a, 0, 1);
            return (n > 0) ? (a[0] & 0xff) : -1;
        }
        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            if (!SocketReadEvent.enabled()) {
                return implRead(b, off, len);
            }
            long start = SocketReadEvent.timestamp();
            int nbytes = implRead(b, off, len);
            SocketReadEvent.offer(start, nbytes, parent.getRemoteSocketAddress(), getSoTimeout());
            return nbytes;
        }

        private int implRead(byte[] b, int off, int len) throws IOException {
            try {
                return in.read(b, off, len);
            } catch (SocketTimeoutException e) {
                throw e;
            } catch (InterruptedIOException e) {
                Thread thread = Thread.currentThread();
                if (thread.isVirtual() && thread.isInterrupted()) {
                    close();
                    throw new SocketException("Closed by interrupt");
                }
                throw e;
            }
        }

        private int getSoTimeout() {
            try {
                return parent.getSoTimeout();
            } catch (SocketException e) {
                // ignored - avoiding exceptions in jfr event data gathering
            }
            return 0;
        }

        @Override
        public int available() throws IOException {
            return in.available();
        }
        @Override
        public void close() throws IOException {
            parent.close();
        }
    }

    /**
     * Returns an output stream for this socket.
     *
     * <p> If this socket has an associated channel then the resulting output
     * stream delegates all of its operations to the channel.  If the channel
     * is in non-blocking mode then the output stream's {@code write}
     * operations will throw an {@link
     * java.nio.channels.IllegalBlockingModeException}.
     *
     * <p> Writing to the output stream is {@linkplain Thread#interrupt()
     * interruptible} in the following circumstances:
     * <ol>
     *   <li> The socket is {@linkplain SocketChannel#socket() associated} with
     *        a {@link SocketChannel SocketChannel}.
     *        In that case, interrupting a thread writing to the output stream
     *        will close the underlying channel and cause the write method to
     *        throw {@link ClosedByInterruptException} with the interrupt status
     *        set.
     *   <li> The socket uses the system-default socket implementation and a
     *        {@linkplain Thread#isVirtual() virtual thread} is writing to the
     *        output stream. In that case, interrupting the virtual thread will
     *        cause it to wakeup and close the socket. The write method will then
     *        throw {@code SocketException} with the interrupt status set.
     * </ol>
     *
     * <p> Closing the returned {@link java.io.OutputStream OutputStream}
     * will close the associated socket.
     *
     * @return     an output stream for writing bytes to this socket.
     * @throws IOException  if an I/O error occurs when creating the
     *         output stream, the socket is not connected or the socket is closed.
     */
    public OutputStream getOutputStream() throws IOException {
        int s = state;
        if (isClosed(s))
            throw new SocketException("Socket is closed");
        if (!isConnected(s))
            throw new SocketException("Socket is not connected");
        if (isOutputShutdown(s))
            throw new SocketException("Socket output is shutdown");
        OutputStream out = this.out;
        if (out == null) {
            // wrap the output stream so that the close method closes this socket
            out = new SocketOutputStream(this, impl.getOutputStream());
            if (!OUT.compareAndSet(this, null, out)) {
                out = this.out;
            }
        }
        return out;
    }

    /**
     * An OutputStream that delegates write operations to an underlying output
     * stream. The close method is overridden to close the Socket.
     */
    private static class SocketOutputStream extends OutputStream {
        private final Socket parent;
        private final OutputStream out;
        SocketOutputStream(Socket parent, OutputStream out) {
            this.parent = parent;
            this.out = out;
        }
        @Override
        public void write(int b) throws IOException {
            byte[] a = new byte[] { (byte) b };
            write(a, 0, 1);
        }
        @Override
        public void write(byte[] b, int off, int len) throws IOException {
            if (!SocketWriteEvent.enabled()) {
                implWrite(b, off, len);
                return;
            }
            long start = SocketWriteEvent.timestamp();
            implWrite(b, off, len);
            SocketWriteEvent.offer(start, len, parent.getRemoteSocketAddress());
        }

        private void implWrite(byte[] b, int off, int len) throws IOException {
            try {
                out.write(b, off, len);
            } catch (InterruptedIOException e) {
                Thread thread = Thread.currentThread();
                if (thread.isVirtual() && thread.isInterrupted()) {
                    close();
                    throw new SocketException("Closed by interrupt");
                }
                throw e;
            }
        }
        @Override
        public void close() throws IOException {
            parent.close();
        }
    }

    /**
     * Enable/disable {@link StandardSocketOptions#TCP_NODELAY TCP_NODELAY}
     * (disable/enable Nagle's algorithm).
     *
     * @param on {@code true} to enable {@code TCP_NODELAY},
     * {@code false} to disable.
     *
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     *
     * @since   1.1
     *
     * @see #getTcpNoDelay()
     */
    public void setTcpNoDelay(boolean on) throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        getImpl().setOption(SocketOptions.TCP_NODELAY, Boolean.valueOf(on));
    }

    /**
     * Tests if {@link StandardSocketOptions#TCP_NODELAY TCP_NODELAY} is enabled.
     *
     * @return a {@code boolean} indicating whether or not
     *         {@code TCP_NODELAY} is enabled.
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @since   1.1
     * @see #setTcpNoDelay(boolean)
     */
    public boolean getTcpNoDelay() throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        return ((Boolean) getImpl().getOption(SocketOptions.TCP_NODELAY)).booleanValue();
    }

    /**
     * Enable/disable {@link StandardSocketOptions#SO_LINGER SO_LINGER} with the
     * specified linger time in seconds. The maximum timeout value is platform
     * specific.
     *
     * The setting only affects socket close.
     *
     * @param on     whether or not to linger on.
     * @param linger how long to linger for, if on is true.
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @throws  IllegalArgumentException if the linger value is negative.
     * @since 1.1
     * @see #getSoLinger()
     */
    public void setSoLinger(boolean on, int linger) throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        if (!on) {
            getImpl().setOption(SocketOptions.SO_LINGER, on);
        } else {
            if (linger < 0) {
                throw new IllegalArgumentException("invalid value for SO_LINGER");
            }
            if (linger > 65535)
                linger = 65535;
            getImpl().setOption(SocketOptions.SO_LINGER, linger);
        }
    }

    /**
     * Returns setting for {@link StandardSocketOptions#SO_LINGER SO_LINGER}.
     * -1 returns implies that the
     * option is disabled.
     *
     * The setting only affects socket close.
     *
     * @return the setting for {@code SO_LINGER}.
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @since   1.1
     * @see #setSoLinger(boolean, int)
     */
    public int getSoLinger() throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        Object o = getImpl().getOption(SocketOptions.SO_LINGER);
        if (o instanceof Integer i) {
            return i.intValue();
        } else {
            return -1;
        }
    }

    /**
     * Send one byte of urgent data on the socket. The byte to be sent is the lowest eight
     * bits of the data parameter. The urgent byte is
     * sent after any preceding writes to the socket OutputStream
     * and before any future writes to the OutputStream.
     * @param data The byte of data to send
     * @throws    IOException if there is an error
     *  sending the data.
     * @since 1.4
     */
    public void sendUrgentData(int data) throws IOException {
        if (!getImpl().supportsUrgentData()) {
            throw new SocketException ("Urgent data not supported");
        }
        getImpl().sendUrgentData (data);
    }

    /**
     * Enable/disable {@link SocketOptions#SO_OOBINLINE SO_OOBINLINE}
     * (receipt of TCP urgent data)
     *
     * By default, this option is disabled and TCP urgent data received on a
     * socket is silently discarded. If the user wishes to receive urgent data, then
     * this option must be enabled. When enabled, urgent data is received
     * inline with normal data.
     * <p>
     * Note, only limited support is provided for handling incoming urgent
     * data. In particular, no notification of incoming urgent data is provided
     * and there is no capability to distinguish between normal data and urgent
     * data unless provided by a higher level protocol.
     *
     * @param on {@code true} to enable {@code SO_OOBINLINE},
     *           {@code false} to disable.
     *
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     *
     * @since   1.4
     *
     * @see #getOOBInline()
     */
    public void setOOBInline(boolean on) throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        getImpl().setOption(SocketOptions.SO_OOBINLINE, Boolean.valueOf(on));
    }

    /**
     * Tests if {@link SocketOptions#SO_OOBINLINE SO_OOBINLINE} is enabled.
     *
     * @return a {@code boolean} indicating whether or not
     *         {@code SO_OOBINLINE} is enabled.
     *
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @since   1.4
     * @see #setOOBInline(boolean)
     */
    public boolean getOOBInline() throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        return ((Boolean) getImpl().getOption(SocketOptions.SO_OOBINLINE)).booleanValue();
    }

    /**
     *  Enable/disable {@link SocketOptions#SO_TIMEOUT SO_TIMEOUT}
     *  with the specified timeout, in milliseconds. With this option set
     *  to a positive timeout value, a read() call on the InputStream associated with
     *  this Socket will block for only this amount of time.  If the timeout
     *  expires, a <B>java.net.SocketTimeoutException</B> is raised, though the
     *  Socket is still valid. A timeout of zero is interpreted as an infinite timeout.
     *  The option <B>must</B> be enabled prior to entering the blocking operation
     *  to have effect.
     *
     * @param timeout the specified timeout, in milliseconds.
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @throws  IllegalArgumentException if {@code timeout} is negative
     * @since   1.1
     * @see #getSoTimeout()
     */
    public void setSoTimeout(int timeout) throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        if (timeout < 0)
            throw new IllegalArgumentException("timeout can't be negative");
        getImpl().setOption(SocketOptions.SO_TIMEOUT, timeout);
    }

    /**
     * Returns setting for {@link SocketOptions#SO_TIMEOUT SO_TIMEOUT}.
     * 0 returns implies that the option is disabled (i.e., timeout of infinity).
     *
     * @return the setting for {@code SO_TIMEOUT}
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     *
     * @since   1.1
     * @see #setSoTimeout(int)
     */
    public int getSoTimeout() throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        Object o = getImpl().getOption(SocketOptions.SO_TIMEOUT);
        /* extra type safety */
        if (o instanceof Integer i) {
            return i.intValue();
        } else {
            return 0;
        }
    }

    /**
     * Sets the {@link StandardSocketOptions#SO_SNDBUF SO_SNDBUF} option to the
     * specified value for this {@code Socket}.
     * The {@code SO_SNDBUF} option is used by the platform's networking code
     * as a hint for the size to set the underlying network I/O buffers.
     *
     * <p>Because {@code SO_SNDBUF} is a hint, applications that want to verify
     * what size the buffers were set to should call {@link #getSendBufferSize()}.
     *
     * @param size the size to which to set the send buffer
     * size. This value must be greater than 0.
     *
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @throws  IllegalArgumentException if the value is 0 or is negative.
     *
     * @see #getSendBufferSize()
     * @since 1.2
     */
    public void setSendBufferSize(int size) throws SocketException {
        if (size <= 0)
            throw new IllegalArgumentException("negative send size");
        if (isClosed())
            throw new SocketException("Socket is closed");
        getImpl().setOption(SocketOptions.SO_SNDBUF, size);
    }

    /**
     * Get value of the {@link StandardSocketOptions#SO_SNDBUF SO_SNDBUF} option
     * for this {@code Socket}, that is the buffer size used by the platform
     * for output on this {@code Socket}.
     * @return the value of the {@code SO_SNDBUF} option for this {@code Socket}.
     *
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     *
     * @see #setSendBufferSize(int)
     * @since 1.2
     */
    public int getSendBufferSize() throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        int result = 0;
        Object o = getImpl().getOption(SocketOptions.SO_SNDBUF);
        if (o instanceof Integer i) {
            result = i.intValue();
        }
        return result;
    }

    /**
     * Sets the {@link StandardSocketOptions#SO_RCVBUF SO_RCVBUF} option to the
     * specified value for this {@code Socket}. The
     * {@code SO_RCVBUF} option is used by the platform's networking code
     * as a hint for the size to set the underlying network I/O buffers.
     *
     * <p>Increasing the receive buffer size can increase the performance of
     * network I/O for high-volume connection, while decreasing it can
     * help reduce the backlog of incoming data.
     *
     * <p>Because {@code SO_RCVBUF} is a hint, applications that want to verify
     * what size the buffers were set to should call {@link #getReceiveBufferSize()}.
     *
     * <p>The value of {@code SO_RCVBUF} is also used to set the TCP receive window
     * that is advertised to the remote peer.
     * Generally, the window size can be modified at any time when a socket is
     * connected. However, if a receive window larger than 64K is required then
     * this must be requested <B>before</B> the socket is connected to the
     * remote peer. There are two cases to be aware of:
     * <ol>
     * <li>For sockets accepted from a ServerSocket, this must be done by calling
     * {@link ServerSocket#setReceiveBufferSize(int)} before the ServerSocket
     * is bound to a local address.</li>
     * <li>For client sockets, setReceiveBufferSize() must be called before
     * connecting the socket to its remote peer.</li></ol>
     * @param size the size to which to set the receive buffer
     * size. This value must be greater than 0.
     *
     * @throws    IllegalArgumentException if the value is 0 or is
     * negative.
     *
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     *
     * @see #getReceiveBufferSize()
     * @see ServerSocket#setReceiveBufferSize(int)
     * @since 1.2
     */
    public void setReceiveBufferSize(int size) throws SocketException {
        if (size <= 0)
            throw new IllegalArgumentException("invalid receive size");
        if (isClosed())
            throw new SocketException("Socket is closed");
        getImpl().setOption(SocketOptions.SO_RCVBUF, size);
    }

    /**
     * Gets the value of the {@link StandardSocketOptions#SO_RCVBUF SO_RCVBUF} option
     * for this {@code Socket}, that is the buffer size used by the platform
     * for input on this {@code Socket}.
     *
     * @return the value of the {@code SO_RCVBUF} option for this {@code Socket}.
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @see #setReceiveBufferSize(int)
     * @since 1.2
     */
    public int getReceiveBufferSize() throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        int result = 0;
        Object o = getImpl().getOption(SocketOptions.SO_RCVBUF);
        if (o instanceof Integer i) {
            result = i.intValue();
        }
        return result;
    }

    /**
     * Enable/disable {@link StandardSocketOptions#SO_KEEPALIVE SO_KEEPALIVE}.
     *
     * @param on  whether or not to have socket keep alive turned on.
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @since 1.3
     * @see #getKeepAlive()
     */
    public void setKeepAlive(boolean on) throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        getImpl().setOption(SocketOptions.SO_KEEPALIVE, Boolean.valueOf(on));
    }

    /**
     * Tests if {@link StandardSocketOptions#SO_KEEPALIVE SO_KEEPALIVE} is enabled.
     *
     * @return a {@code boolean} indicating whether or not
     *         {@code SO_KEEPALIVE} is enabled.
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @since   1.3
     * @see #setKeepAlive(boolean)
     */
    public boolean getKeepAlive() throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        return ((Boolean) getImpl().getOption(SocketOptions.SO_KEEPALIVE)).booleanValue();
    }

    /**
     * Sets traffic class or type-of-service octet in the IP
     * header for packets sent from this Socket.
     * As the underlying network implementation may ignore this
     * value applications should consider it a hint.
     *
     * <P> The tc <B>must</B> be in the range {@code 0 <= tc <=
     * 255} or an IllegalArgumentException will be thrown.
     * <p>Notes:
     * <p>For Internet Protocol v4 the value consists of an
     * {@code integer}, the least significant 8 bits of which
     * represent the value of the TOS octet in IP packets sent by
     * the socket.
     * RFC 1349 defines the TOS values as follows:
     *
     * <UL>
     * <LI><CODE>IPTOS_LOWCOST (0x02)</CODE></LI>
     * <LI><CODE>IPTOS_RELIABILITY (0x04)</CODE></LI>
     * <LI><CODE>IPTOS_THROUGHPUT (0x08)</CODE></LI>
     * <LI><CODE>IPTOS_LOWDELAY (0x10)</CODE></LI>
     * </UL>
     * The last low order bit is always ignored as this
     * corresponds to the MBZ (must be zero) bit.
     * <p>
     * Setting bits in the precedence field may result in a
     * SocketException indicating that the operation is not
     * permitted.
     * <p>
     * As RFC 1122 section 4.2.4.2 indicates, a compliant TCP
     * implementation should, but is not required to, let application
     * change the TOS field during the lifetime of a connection.
     * So whether the type-of-service field can be changed after the
     * TCP connection has been established depends on the implementation
     * in the underlying platform. Applications should not assume that
     * they can change the TOS field after the connection.
     * <p>
     * For Internet Protocol v6 {@code tc} is the value that
     * would be placed into the sin6_flowinfo field of the IP header.
     *
     * @param tc        an {@code int} value for the bitset.
     * @throws SocketException if there is an error setting the traffic class or type-of-service,
     *         or the socket is closed.
     * @since 1.4
     * @see #getTrafficClass
     * @see StandardSocketOptions#IP_TOS
     */
    public void setTrafficClass(int tc) throws SocketException {
        if (tc < 0 || tc > 255)
            throw new IllegalArgumentException("tc is not in range 0 -- 255");
        if (isClosed())
            throw new SocketException("Socket is closed");
        getImpl().setOption(SocketOptions.IP_TOS, tc);
    }

    /**
     * Gets traffic class or type-of-service in the IP header
     * for packets sent from this Socket
     * <p>
     * As the underlying network implementation may ignore the
     * traffic class or type-of-service set using {@link #setTrafficClass(int)}
     * this method may return a different value than was previously
     * set using the {@link #setTrafficClass(int)} method on this Socket.
     *
     * @return the traffic class or type-of-service already set
     * @throws SocketException if there is an error obtaining the traffic class
     *         or type-of-service value, or the socket is closed.
     * @since 1.4
     * @see #setTrafficClass(int)
     * @see StandardSocketOptions#IP_TOS
     */
    public int getTrafficClass() throws SocketException {
        return ((Integer) (getImpl().getOption(SocketOptions.IP_TOS))).intValue();
    }

    /**
     * Enable/disable the {@link StandardSocketOptions#SO_REUSEADDR SO_REUSEADDR}
     * socket option.
     * <p>
     * When a TCP connection is closed the connection may remain
     * in a timeout state for a period of time after the connection
     * is closed (typically known as the {@code TIME_WAIT} state
     * or {@code 2MSL} wait state).
     * For applications using a well known socket address or port
     * it may not be possible to bind a socket to the required
     * {@code SocketAddress} if there is a connection in the
     * timeout state involving the socket address or port.
     * <p>
     * Enabling {@code SO_REUSEADDR} prior to binding the socket using
     * {@link #bind(SocketAddress)} allows the socket to be bound even
     * though a previous connection is in a timeout state.
     * <p>
     * When a {@code Socket} is created the initial setting
     * of {@code SO_REUSEADDR} is disabled.
     * <p>
     * The behaviour when {@code SO_REUSEADDR} is enabled or disabled after
     * a socket is bound (See {@link #isBound()}) is not defined.
     *
     * @param on  whether to enable or disable the socket option
     * @throws    SocketException if an error occurs enabling or
     *            disabling the {@code SO_REUSEADDR}
     *            socket option, or the socket is closed.
     * @since 1.4
     * @see #getReuseAddress()
     * @see #bind(SocketAddress)
     * @see #isClosed()
     * @see #isBound()
     */
    public void setReuseAddress(boolean on) throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        getImpl().setOption(SocketOptions.SO_REUSEADDR, Boolean.valueOf(on));
    }

    /**
     * Tests if {@link StandardSocketOptions#SO_REUSEADDR SO_REUSEADDR} is enabled.
     *
     * @return a {@code boolean} indicating whether or not
     *         {@code SO_REUSEADDR} is enabled.
     * @throws SocketException if there is an error in the underlying protocol,
     *         such as a TCP error, or the socket is closed.
     * @since   1.4
     * @see #setReuseAddress(boolean)
     */
    public boolean getReuseAddress() throws SocketException {
        if (isClosed())
            throw new SocketException("Socket is closed");
        return ((Boolean) (getImpl().getOption(SocketOptions.SO_REUSEADDR))).booleanValue();
    }

    private void closeSuppressingExceptions(Throwable parentException) {
        try {
            close();
        } catch (IOException exception) {
            parentException.addSuppressed(exception);
        }
    }

    /**
     * Closes this socket.
     * <p>
     * Any thread currently blocked in an I/O operation upon this socket
     * will throw a {@link SocketException}.
     * <p>
     * Once a socket has been closed, it is not available for further networking
     * use (i.e. can't be reconnected or rebound) and several of the methods defined
     * by this class will throw an exception if invoked on the closed socket. A new
     * socket needs to be created.
     *
     * <p> Closing this socket will also close the socket's
     * {@link java.io.InputStream InputStream} and
     * {@link java.io.OutputStream OutputStream}.
     *
     * <p> If this socket has an associated channel then the channel is closed
     * as well.
     *
     * @throws     IOException  if an I/O error occurs when closing this socket.
     * @see #isClosed()
     */
    public void close() throws IOException {
        synchronized (socketLock) {
            if ((state & CLOSED) == 0) {
                int s = getAndBitwiseOrState(CLOSED);
                if ((s & (SOCKET_CREATED | CLOSED)) == SOCKET_CREATED) {
                    // close underlying socket if created
                    impl.close();
                }
            }
        }
    }

    /**
     * Places the input stream for this socket at "end of stream".
     * Any data sent to the input stream side of the socket is acknowledged
     * and then silently discarded.
     * <p>
     * If you read from a socket input stream after invoking this method on the
     * socket, the stream's {@code available} method will return 0, and its
     * {@code read} methods will return {@code -1} (end of stream).
     *
     * @throws IOException if an I/O error occurs when shutting down this socket, the
     *         socket is not connected or the socket is closed.
     *
     * @since 1.3
     * @see java.net.Socket#shutdownOutput()
     * @see java.net.Socket#close()
     * @see java.net.Socket#setSoLinger(boolean, int)
     * @see #isInputShutdown()
     */
    public void shutdownInput() throws IOException {
        int s = state;
        if (isClosed(s))
            throw new SocketException("Socket is closed");
        if (!isConnected(s))
            throw new SocketException("Socket is not connected");
        if (isInputShutdown(s))
            throw new SocketException("Socket input is already shutdown");
        getImpl().shutdownInput();
        getAndBitwiseOrState(SHUT_IN);
    }

    /**
     * Disables the output stream for this socket.
     * For a TCP socket, any previously written data will be sent
     * followed by TCP's normal connection termination sequence.
     *
     * If you write to a socket output stream after invoking
     * shutdownOutput() on the socket, the stream will throw
     * an IOException.
     *
     * @throws IOException if an I/O error occurs when shutting down this socket, the socket
     *         is not connected or the socket is closed.
     *
     * @since 1.3
     * @see java.net.Socket#shutdownInput()
     * @see java.net.Socket#close()
     * @see java.net.Socket#setSoLinger(boolean, int)
     * @see #isOutputShutdown()
     */
    public void shutdownOutput() throws IOException {
        int s = state;
        if (isClosed(s))
            throw new SocketException("Socket is closed");
        if (!isConnected(s))
            throw new SocketException("Socket is not connected");
        if (isOutputShutdown(s))
            throw new SocketException("Socket output is already shutdown");
        getImpl().shutdownOutput();
        getAndBitwiseOrState(SHUT_OUT);
    }

    /**
     * Converts this socket to a {@code String}.
     *
     * @return  a string representation of this socket.
     */
    public String toString() {
        try {
            if (isConnected())
                return "Socket[addr=" + getImpl().getInetAddress() +
                    ",port=" + getImpl().getPort() +
                    ",localport=" + getImpl().getLocalPort() + "]";
        } catch (SocketException e) {
        }
        return "Socket[unconnected]";
    }

    /**
     * Returns the connection state of the socket.
     * <p>
     * Note: Closing a socket doesn't clear its connection state, which means
     * this method will return {@code true} for a closed socket
     * (see {@link #isClosed()}) if it was successfully connected prior
     * to being closed.
     *
     * @return true if the socket was successfully connected to a server
     * @since 1.4
     */
    public boolean isConnected() {
        return isConnected(state);
    }

    /**
     * Returns the binding state of the socket.
     * <p>
     * Note: Closing a socket doesn't clear its binding state, which means
     * this method will return {@code true} for a closed socket
     * (see {@link #isClosed()}) if it was successfully bound prior
     * to being closed.
     *
     * @return true if the socket was successfully bound to an address
     * @since 1.4
     * @see #bind
     */
    public boolean isBound() {
        return isBound(state);
    }

    /**
     * Returns the closed state of the socket.
     *
     * @return true if the socket has been closed
     * @since 1.4
     * @see #close
     */
    public boolean isClosed() {
        return isClosed(state);
    }

    /**
     * Returns whether the read-half of the socket connection is closed.
     *
     * @return true if the input of the socket has been shutdown
     * @since 1.4
     * @see #shutdownInput
     */
    public boolean isInputShutdown() {
        return isInputShutdown(state);
    }

    /**
     * Returns whether the write-half of the socket connection is closed.
     *
     * @return true if the output of the socket has been shutdown
     * @since 1.4
     * @see #shutdownOutput
     */
    public boolean isOutputShutdown() {
        return isOutputShutdown(state);
    }

    /**
     * The factory for all client sockets.
     */
    private static volatile SocketImplFactory factory;

    static SocketImplFactory socketImplFactory() {
        return factory;
    }

    /**
     * Sets the client socket implementation factory for the
     * application. The factory can be specified only once.
     * <p>
     * When an application creates a new client socket, the socket
     * implementation factory's {@code createSocketImpl} method is
     * called to create the actual socket implementation.
     * <p>
     * Passing {@code null} to the method is a no-op unless the factory
     * was already set.
     *
     * @param      fac   the desired factory.
     * @throws     IOException  if an I/O error occurs when setting the
     *               socket factory.
     * @throws     SocketException  if the factory is already defined.
     * @see        java.net.SocketImplFactory#createSocketImpl()
     * @deprecated Use a {@link javax.net.SocketFactory} and subclass {@code Socket}
     *    directly.
     *    <br> This method provided a way in early JDK releases to replace the
     *    system wide implementation of {@code Socket}. It has been mostly
     *    obsolete since Java 1.4. If required, a {@code Socket} can be
     *    created to use a custom implementation by extending {@code Socket}
     *    and using the {@linkplain #Socket(SocketImpl) protected
     *    constructor} that takes an {@linkplain SocketImpl implementation}
     *    as a parameter.
     */
    @Deprecated(since = "17")
    public static synchronized void setSocketImplFactory(SocketImplFactory fac)
        throws IOException
    {
        if (factory != null) {
            throw new SocketException("factory already defined");
        }
        factory = fac;
    }

    /**
     * Sets performance preferences for this socket.
     *
     * <p> Sockets use the TCP/IP protocol by default.  Some implementations
     * may offer alternative protocols which have different performance
     * characteristics than TCP/IP.  This method allows the application to
     * express its own preferences as to how these tradeoffs should be made
     * when the implementation chooses from the available protocols.
     *
     * <p> Performance preferences are described by three integers
     * whose values indicate the relative importance of short connection time,
     * low latency, and high bandwidth.  The absolute values of the integers
     * are irrelevant; in order to choose a protocol the values are simply
     * compared, with larger values indicating stronger preferences. Negative
     * values represent a lower priority than positive values. If the
     * application prefers short connection time over both low latency and high
     * bandwidth, for example, then it could invoke this method with the values
     * {@code (1, 0, 0)}.  If the application prefers high bandwidth above low
     * latency, and low latency above short connection time, then it could
     * invoke this method with the values {@code (0, 1, 2)}.
     *
     * <p> Invoking this method after this socket has been connected
     * will have no effect.
     *
     * @param  connectionTime
     *         An {@code int} expressing the relative importance of a short
     *         connection time
     *
     * @param  latency
     *         An {@code int} expressing the relative importance of low
     *         latency
     *
     * @param  bandwidth
     *         An {@code int} expressing the relative importance of high
     *         bandwidth
     *
     * @since 1.5
     */
    public void setPerformancePreferences(int connectionTime,
                                          int latency,
                                          int bandwidth)
    {
        /* Not implemented yet */
    }


    /**
     * Sets the value of a socket option.
     *
     * @param <T> The type of the socket option value
     * @param name The socket option
     * @param value The value of the socket option. A value of {@code null}
     *              may be valid for some options.
     * @return this Socket
     *
     * @throws UnsupportedOperationException if the socket does not support
     *         the option.
     *
     * @throws IllegalArgumentException if the value is not valid for
     *         the option.
     *
     * @throws IOException if an I/O error occurs, or if the socket is closed.
     *
     * @throws NullPointerException if name is {@code null}
     *
     * @since 9
     */
    public <T> Socket setOption(SocketOption<T> name, T value) throws IOException {
        Objects.requireNonNull(name);
        if (isClosed())
            throw new SocketException("Socket is closed");
        getImpl().setOption(name, value);
        return this;
    }

    /**
     * Returns the value of a socket option.
     *
     * @param <T> The type of the socket option value
     * @param name The socket option
     *
     * @return The value of the socket option.
     *
     * @throws UnsupportedOperationException if the socket does not support
     *         the option.
     *
     * @throws IOException if an I/O error occurs, or if the socket is closed.
     *
     * @throws NullPointerException if name is {@code null}
     *
     * @since 9
     */
    public <T> T getOption(SocketOption<T> name) throws IOException {
        Objects.requireNonNull(name);
        if (isClosed())
            throw new SocketException("Socket is closed");
        return getImpl().getOption(name);
    }

    // cache of unmodifiable impl options. Possibly set racy, in impl we trust
    private volatile Set<SocketOption<?>> options;

    /**
     * Returns a set of the socket options supported by this socket.
     *
     * This method will continue to return the set of options even after
     * the socket has been closed.
     *
     * @return A set of the socket options supported by this socket. This set
     *         may be empty if the socket's SocketImpl cannot be created.
     *
     * @since 9
     */
    public Set<SocketOption<?>> supportedOptions() {
        Set<SocketOption<?>> so = options;
        if (so != null)
            return so;

        try {
            SocketImpl impl = getImpl();
            options = Collections.unmodifiableSet(impl.supportedOptions());
        } catch (IOException e) {
            options = Collections.emptySet();
        }
        return options;
    }
}
