/*
 * Copyright (c) 2016, 2018, Oracle and/or its affiliates. All rights reserved.
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

#include <jni.h>
#include <string.h>

#include "net_util.h"
#include "net_util_md.h"
#include "java_net_SocketCleanable.h"

JNIEXPORT jboolean JNICALL
Java_java_net_AbstractPlainSocketImpl_isReusePortAvailable0(JNIEnv* env, jclass c1)
{
    return (reuseport_available()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_java_net_AbstractPlainDatagramSocketImpl_isReusePortAvailable0(JNIEnv* env, jclass c1)
{
    return (reuseport_available()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_jdk_net_Sockets_isReusePortAvailable0(JNIEnv* env, jclass c1)
{
    return (reuseport_available()) ? JNI_TRUE : JNI_FALSE;
}

/*
 * Class:     java_net_SocketCleanable
 * Method:    cleanupClose0
 * Signature: (I)V
 */
JNIEXPORT void JNICALL
Java_java_net_SocketCleanable_cleanupClose0(JNIEnv *env, jclass c1, jint fd)
{
    NET_SocketClose(fd);
}

static jclass isa_class = NULL;
static jmethodID isa_ctor = NULL;

JNIEXPORT void JNICALL
Java_java_net_Socket_initNative(JNIEnv *env, jclass c1)
{
    jclass isa_class_local = (*env)->FindClass(env, "java/net/InetSocketAddress");
    if (isa_class_local == NULL) {
        JNU_ThrowClassNotFoundException(env, "java.net.InetSocketAddress");
        return;
    }
    isa_class = (*env)->NewGlobalRef(env, isa_class_local);
    isa_ctor = (*env)->GetMethodID(env, isa_class, "<init>", "(Ljava/net/InetAddress;I)V");
    if (isa_ctor == NULL) {
        JNU_ThrowByName(env, "java/lang/NoSuchMethodError", "InetSocketAddress.<init>(java.net.InetAddress, int)");
    }
}

static jobject create_isa(JNIEnv *env, jclass isa_class, jmethodID isa_ctor, SOCKETADDRESS *addr) {
    jint port;
    jobject inetAddr = NET_SockaddrToInetAddress(env, addr, &port);
    return (*env)->NewObject(env, isa_class, isa_ctor, inetAddr, port);
}

JNIEXPORT jobjectArray JNICALL
Java_java_net_Socket_getAddresses(JNIEnv *env, jclass cl, jint fd) {
    int family;
    socklen_t famlen = sizeof(int);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, &famlen) != 0) {
        JNU_ThrowByName(env, "java/net/SocketException", "Cannot find socket family");
        return NULL;
    } else if (family != AF_INET && family != AF_INET6) {
        return NULL;
    }

    jobjectArray arr = (*env)->NewObjectArray(env, 2, isa_class, NULL);
    if (arr == NULL) {
        JNU_ThrowOutOfMemoryError(env, "java.net.InetSocketAddres[2]");
        return NULL;
    }

    SOCKETADDRESS local;
    socklen_t llen = sizeof(SOCKETADDRESS);
    if (getsockname(fd, &local.sa, &llen) != 0) {
        JNU_ThrowIllegalArgumentException(env, strerror(errno));
        return NULL;
    }
    jobject localAddr = create_isa(env, isa_class, isa_ctor, &local);
    if (localAddr == NULL) {
        JNU_ThrowOutOfMemoryError(env, "java.net.InetSocketAddres");
        return NULL;
    }

    jobject remoteAddr;
    SOCKETADDRESS remote;
    socklen_t rlen = sizeof(SOCKETADDRESS);
    if (getpeername(fd, &remote.sa, &rlen) != 0) {
        if (errno == ENOTCONN) {
            remoteAddr = NULL;
        } else {
            JNU_ThrowIllegalArgumentException(env, strerror(errno));
            return NULL;
        }
    } else {
        remoteAddr = create_isa(env, isa_class, isa_ctor, &remote);
        if (remoteAddr == NULL) {
           JNU_ThrowOutOfMemoryError(env, "java.net.InetSocketAddres");
           return NULL;
        }
    }

    (*env)->SetObjectArrayElement(env, arr, 0, localAddr);
    (*env)->SetObjectArrayElement(env, arr, 1, remoteAddr);
    return arr;
}

JNIEXPORT jstring JNICALL
Java_java_net_Socket_getType(JNIEnv *env, jclass cl, jint fd) {
    int socktype, family;
    socklen_t typelen = sizeof(int), famlen = sizeof(int);
    const char *type;
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, &famlen) != 0) {
        JNU_ThrowByName(env, "java/net/SocketException", "Cannot find socket family");
        return NULL;
    } else if (family == AF_UNIX) {
        type = "unix socket";
    } else if (family != AF_INET && family != AF_INET6) {
        type = "unknown socket family";
    } else {
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &socktype, &typelen) != 0) {
            JNU_ThrowByName(env, "java/net/SocketException", "Cannot find socket type");
            return NULL;
        }
        switch(socktype) {
            case SOCK_STREAM:
                type = family == AF_INET ? "tcp" : "tcp6";
                break;
            case SOCK_DGRAM:
                type = family == AF_INET ? "udp" : "udp6";
                break;
            case SOCK_RAW:
                type = family == AF_INET ? "raw" : "raw6";
                break;
            default:
                type = family == AF_INET ? "unknown IPv4" : "unknown IPv6";
        }
    }
    return (*env)->NewStringUTF(env, type);
}