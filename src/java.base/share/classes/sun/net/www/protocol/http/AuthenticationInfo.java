/*
 * Copyright (c) 1995, 2024, Oracle and/or its affiliates. All rights reserved.
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

package sun.net.www.protocol.http;

import java.net.PasswordAuthentication;
import java.net.URL;
import java.util.HashMap;
import java.util.Locale;
import java.util.Objects;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.ReentrantLock;
import java.util.function.BiFunction;

import sun.net.www.HeaderParser;


/**
 * AuthenticationInfo: Encapsulate the information needed to
 * authenticate a user to a server.
 *
 * @author Jon Payne
 * @author Herb Jellinek
 * @author Bill Foote
 */
// REMIND:  It would be nice if this class understood about partial matching.
//      If you're authorized for foo.com, chances are high you're also
//      authorized for baz.foo.com.
// NB:  When this gets implemented, be careful about the uncaching
//      policy in HttpURLConnection.  A failure on baz.foo.com shouldn't
//      uncache foo.com!

public abstract class AuthenticationInfo extends AuthCacheValue implements Cloneable {

    // Constants saying what kind of authorization this is.  This determines
    // the namespace in the hash table lookup.
    public static final char SERVER_AUTHENTICATION = 's';
    public static final char PROXY_AUTHENTICATION = 'p';

    /**
     * If true, then simultaneous authentication requests to the same realm/proxy
     * are serialized, in order to avoid a user having to type the same username/passwords
     * repeatedly, via the Authenticator. Default is false, which means that this
     * behavior is switched off.
     */
    static final boolean serializeAuth = Boolean.getBoolean("http.auth.serializeRequests");

    /* AuthCacheValue: */

    protected PasswordAuthentication pw;

    public PasswordAuthentication credentials() {
        return pw;
    }

    public AuthCacheValue.Type getAuthType() {
        return type == SERVER_AUTHENTICATION ?
            AuthCacheValue.Type.Server:
            AuthCacheValue.Type.Proxy;
    }

    AuthScheme getAuthScheme() {
        return authScheme;
    }

    public String getHost() {
        return host;
    }
    public int getPort() {
        return port;
    }
    public String getRealm() {
        return realm;
    }
    public String getPath() {
        return path;
    }
    public String getProtocolScheme() {
        return protocol;
    }
    /**
     * Whether we should cache this instance in the AuthCache.
     * This method returns {@code true} by default.
     * Subclasses may override this method to add
     * additional restrictions.
     * @return {@code true} by default.
     */
    protected boolean useAuthCache() {
        return true;
    }

    /**
     * requests is used to ensure that interaction with the
     * Authenticator for a particular realm is single threaded.
     * i.e. if multiple threads need to get credentials from the user
     * at the same time, then all but the first will block until
     * the first completes its authentication.
     */
    private static final HashMap<String,Thread> requests = new HashMap<>();
    private static final ReentrantLock requestLock = new ReentrantLock();
    private static final Condition requestFinished = requestLock.newCondition();
    /*
     * check if AuthenticationInfo is available in the cache.
     * If not, check if a request for this destination is in progress
     * and if so block until the other request is finished authenticating
     * and returns the cached authentication value.
     * Otherwise, returns the cached authentication value, which may be null.
     */
    private static AuthenticationInfo requestAuthentication(
        String key, AuthCacheImpl acache, BiFunction<String, AuthCacheImpl, AuthenticationInfo> cachefunc)
    {
        AuthenticationInfo cached = cachefunc.apply(key, acache);
        if (cached != null || !serializeAuth) {
            // either we already have a value in the cache, and we can
            // use that immediately, or the serializeAuth behavior is disabled,
            // and we can revert to concurrent requests
            return cached;
        }
        requestLock.lock();
        try {
            // check again after locking, and if available
            // just return the cached value.
            cached = cachefunc.apply(key, acache);
            if (cached != null) return cached;

            // Otherwise, if no request is in progress, record this
            // thread as performing authentication and returns null.
            Thread c = Thread.currentThread();
            Thread t = requests.putIfAbsent(key, c);
            if (t == null || t == c) {
                return null;
            }
            // Otherwise, an other thread is currently performing authentication:
            // wait until it finishes.
            while (requests.containsKey(key)) {
                requestFinished.awaitUninterruptibly();
            }
        } finally {
            requestLock.unlock();
        }
        /* entry may be in cache now. */
        return cachefunc.apply(key, acache);
    }

    /* signal completion of an authentication (whether it succeeded or not)
     * so that other threads can continue.
     */
    private static void requestCompleted (String key) {
        requestLock.lock();
        try {
            Thread thread = requests.get(key);
            if (thread != null && thread == Thread.currentThread()) {
                boolean waspresent = requests.remove(key) != null;
                assert waspresent;
            }
            requestFinished.signalAll();
        } finally {
            requestLock.unlock();
        }
    }

    // REMIND:  This cache just grows forever.  We should put in a bounded
    //          cache, or maybe something using WeakRef's.

    /** The type (server/proxy) of authentication this is.  Used for key lookup */
    char type;

    /** The authentication scheme (basic/digest). Also used for key lookup */
    AuthScheme authScheme;

    /** The protocol/scheme (i.e. http or https ). Need to keep the caches
     *  logically separate for the two protocols. This field is only used
     *  when constructed with a URL (the normal case for server authentication)
     *  For proxy authentication the protocol is not relevant.
     */
    String protocol;

    /** The host we're authenticating against. */
    String host;

    /** The port on the host we're authenticating against. */
    int port;

    /** The realm we're authenticating against. */
    String realm;

    /** The shortest path from the URL we authenticated against. */
    String path;

    /** Use this constructor only for proxy entries */
    public AuthenticationInfo(char type, AuthScheme authScheme, String host,
                              int port, String realm) {
        this.type = type;
        this.authScheme = authScheme;
        this.protocol = "";
        this.host = host.toLowerCase(Locale.ROOT);
        this.port = port;
        this.realm = realm;
        this.path = null;
    }

    public Object clone() {
        try {
            return super.clone ();
        } catch (CloneNotSupportedException e) {
            // Cannot happen because Cloneable implemented by AuthenticationInfo
            return null;
        }
    }

    /*
     * Constructor used to limit the authorization to the path within
     * the URL. Use this constructor for origin server entries.
     */
    public AuthenticationInfo(char type, AuthScheme authScheme, URL url, String realm) {
        this.type = type;
        this.authScheme = authScheme;
        this.protocol = url.getProtocol().toLowerCase(Locale.ROOT);
        this.host = url.getHost().toLowerCase(Locale.ROOT);
        this.port = url.getPort();
        if (this.port == -1) {
            this.port = url.getDefaultPort();
        }
        this.realm = realm;

        String urlPath = url.getPath();
        if (urlPath.isEmpty())
            this.path = urlPath;
        else {
            this.path = reducePath (urlPath);
        }
    }

    /*
     * reduce the path to the root of where we think the
     * authorization begins. This could get shorter as
     * the url is traversed up following a successful challenge.
     */
    static String reducePath (String urlPath) {
        int sepIndex = urlPath.lastIndexOf('/');
        int targetSuffixIndex = urlPath.lastIndexOf('.');
        if (sepIndex != -1)
            if (sepIndex < targetSuffixIndex)
                return urlPath.substring(0, sepIndex+1);
            else
                return urlPath;
        else
            return urlPath;
    }

    /**
     * Returns info for the URL, for an HTTP server auth.  Used when we
     * don't yet know the realm
     * (i.e. when we're preemptively setting the auth).
     */
    static AuthenticationInfo getServerAuth(URL url, AuthCacheImpl cache) {
        int port = url.getPort();
        if (port == -1) {
            port = url.getDefaultPort();
        }
        String key = SERVER_AUTHENTICATION + ":" + url.getProtocol().toLowerCase(Locale.ROOT)
                + ":" + url.getHost().toLowerCase(Locale.ROOT) + ":" + port;
        return getAuth(key, url, cache);
    }

    /**
     * Returns info for the URL, for an HTTP server auth.  Used when we
     * do know the realm (i.e. when we're responding to a challenge).
     * In this case we do not use the path because the protection space
     * is identified by the host:port:realm only
     */
    static String getServerAuthKey(URL url, String realm, AuthScheme scheme) {
        int port = url.getPort();
        if (port == -1) {
            port = url.getDefaultPort();
        }
        String key = SERVER_AUTHENTICATION + ":" + scheme + ":"
                     + url.getProtocol().toLowerCase(Locale.ROOT)
                     + ":" + url.getHost().toLowerCase(Locale.ROOT)
                     + ":" + port + ":" + realm;
        return key;
    }

    private static AuthenticationInfo getCachedServerAuth(String key, AuthCacheImpl cache) {
        return getAuth(key, null, cache);
    }

    static AuthenticationInfo getServerAuth(String key, AuthCacheImpl cache) {
        if (!serializeAuth) return getCachedServerAuth(key, cache);
        return requestAuthentication(key, cache, AuthenticationInfo::getCachedServerAuth);
    }

    /**
     * Return the AuthenticationInfo object from the cache if it's path is
     * a substring of the supplied URLs path.
     */
    static AuthenticationInfo getAuth(String key, URL url, AuthCacheImpl acache) {
        Objects.requireNonNull(acache);
        if (url == null) {
            return (AuthenticationInfo)acache.get (key, null);
        } else {
            return (AuthenticationInfo)acache.get (key, url.getPath());
        }
    }

    /**
     * Returns a firewall authentication, for the given host/port.  Used
     * for preemptive header-setting. Note, the protocol field is always
     * blank for proxies.
     */
    static AuthenticationInfo getProxyAuth(String host, int port, AuthCacheImpl acache) {
        Objects.requireNonNull(acache);
        String key = PROXY_AUTHENTICATION + "::" + host.toLowerCase(Locale.ROOT) + ":" + port;
        AuthenticationInfo result = (AuthenticationInfo) acache.get(key, null);
        return result;
    }

    /**
     * Returns a firewall authentication, for the given host/port and realm.
     * Used in response to a challenge. Note, the protocol field is always
     * blank for proxies.
     */
    static String getProxyAuthKey(String host, int port, String realm, AuthScheme scheme) {
        String key = PROXY_AUTHENTICATION + ":" + scheme
                        + "::" + host.toLowerCase(Locale.ROOT)
                        + ":" + port + ":" + realm;
        return key;
    }

    private static AuthenticationInfo getCachedProxyAuth(String key, AuthCacheImpl acache) {
        Objects.requireNonNull(acache);
        return (AuthenticationInfo) acache.get(key, null);
    }

    static AuthenticationInfo getProxyAuth(String key, AuthCacheImpl acache) {
        if (!serializeAuth) return getCachedProxyAuth(key, acache);
        return requestAuthentication(key, acache, AuthenticationInfo::getCachedProxyAuth);
    }


    /**
     * Add this authentication to the cache
     */
    void addToCache(AuthCacheImpl authcache) {
        Objects.requireNonNull(authcache);
        String key = cacheKey(true);
        if (useAuthCache()) {
            authcache.put(key, this);
            if (supportsPreemptiveAuthorization()) {
                authcache.put(cacheKey(false), this);
            }
        }
        endAuthRequest(key);
    }

    static void endAuthRequest (String key) {
        if (!serializeAuth) {
            return;
        }
        requestCompleted(key);
    }

    /**
     * Remove this authentication from the cache
     */
    void removeFromCache(AuthCacheImpl authcache) {
        Objects.requireNonNull(authcache);
        authcache.remove(cacheKey(true), this);
        if (supportsPreemptiveAuthorization()) {
            authcache.remove(cacheKey(false), this);
        }
    }

    /**
     * @return true if this authentication supports preemptive authorization
     */
    public abstract boolean supportsPreemptiveAuthorization();

    /**
     * @return the name of the HTTP header this authentication wants set.
     *          This is used for preemptive authorization.
     */
    public String getHeaderName() {
        if (type == SERVER_AUTHENTICATION) {
            return "Authorization";
        } else {
            return "Proxy-authorization";
        }
    }

    /**
     * Calculates and returns the authentication header value based
     * on the stored authentication parameters. If the calculation does not depend
     * on the URL or the request method then these parameters are ignored.
     * @param url The URL
     * @param method The request method
     * @return the value of the HTTP header this authentication wants set.
     *          Used for preemptive authorization.
     */
    public abstract String getHeaderValue(URL url, String method);

    /**
     * Set header(s) on the given connection.  Subclasses must override
     * This will only be called for
     * definitive (i.e. non-preemptive) authorization.
     * @param conn The connection to apply the header(s) to
     * @param p A source of header values for this connection, if needed.
     * @param raw The raw header field (if needed)
     * @return true if all goes well, false if no headers were set.
     */
    public abstract boolean setHeaders(HttpURLConnection conn, HeaderParser p, String raw);

    /**
     * Check if the header indicates that the current auth. parameters are stale.
     * If so, then replace the relevant field with the new value
     * and return true. Otherwise return false.
     * returning true means the request can be retried with the same userid/password
     * returning false means we have to go back to the user to ask for a new
     * username password.
     */
    public abstract boolean isAuthorizationStale (String header);

    /**
     * Give a key for hash table lookups.
     * @param includeRealm if you want the realm considered.  Preemptively
     *          setting an authorization is done before the realm is known.
     */
    String cacheKey(boolean includeRealm) {
        // This must be kept in sync with the getXXXAuth() methods in this
        // class.
        if (includeRealm) {
            return type + ":" + authScheme + ":" + protocol + ":"
                        + host + ":" + port + ":" + realm;
        } else {
            return type + ":" + protocol + ":" + host + ":" + port;
        }
    }

    /**
     * Releases any system or cryptographic resources.
     * It is up to implementors to override disposeContext()
     * to take necessary action.
     */
    public void disposeContext() {
        // do nothing
    }
}
