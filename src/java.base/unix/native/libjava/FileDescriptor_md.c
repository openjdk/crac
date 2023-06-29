/*
 * Copyright (c) 1997, 2017, Oracle and/or its affiliates. All rights reserved.
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
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "jni.h"
#include "jni_util.h"
#include "jvm.h"

#include "io_util_md.h"
#include "java_io_FileDescriptor.h"

#include <sys/socket.h>
#include <netinet/in.h>

typedef union {
    struct sockaddr     sa;
    struct sockaddr_in  sa4;
    struct sockaddr_in6 sa6;
} socketaddress;

/*******************************************************************/
/*  BEGIN JNI ********* BEGIN JNI *********** BEGIN JNI ************/
/*******************************************************************/

/* field id for jint 'fd' in java.io.FileDescriptor */
jfieldID IO_fd_fdID;

/* field id for jboolean 'append' in java.io.FileDescriptor */
jfieldID IO_append_fdID;

/**************************************************************
 * static methods to store field ID's in initializers
 */

JNIEXPORT void JNICALL
Java_java_io_FileDescriptor_initIDs(JNIEnv *env, jclass fdClass) {
    CHECK_NULL(IO_fd_fdID = (*env)->GetFieldID(env, fdClass, "fd", "I"));
    CHECK_NULL(IO_append_fdID = (*env)->GetFieldID(env, fdClass, "append", "Z"));
}

/**************************************************************
 * File Descriptor
 */

JNIEXPORT void JNICALL
Java_java_io_FileDescriptor_sync(JNIEnv *env, jobject this) {
    FD fd = THIS_FD(this);
    if (IO_Sync(fd) == -1) {
        JNU_ThrowByName(env, "java/io/SyncFailedException", "sync failed");
    }
}
JNIEXPORT jlong JNICALL
Java_java_io_FileDescriptor_getHandle(JNIEnv *env, jclass fdClass, jint fd) {
    return -1;
}

JNIEXPORT jboolean JNICALL
Java_java_io_FileDescriptor_getAppend(JNIEnv *env, jclass fdClass, jint fd) {
    int flags = fcntl(fd, F_GETFL);
    return ((flags & O_APPEND) == 0) ? JNI_FALSE : JNI_TRUE;
}

// instance method close0 for FileDescriptor
JNIEXPORT void JNICALL
Java_java_io_FileDescriptor_close0(JNIEnv *env, jobject this) {
    fileDescriptorClose(env, this);
}

JNIEXPORT void JNICALL
Java_java_io_FileCleanable_cleanupClose0(JNIEnv *env, jclass fdClass, jint fd, jlong unused) {
    if (fd != -1) {
        if (close(fd) == -1) {
            JNU_ThrowIOExceptionWithLastError(env, "close failed");
        }
    }
}

static const char* stat2strtype(mode_t mode) {
    switch (mode & S_IFMT) {
        case S_IFSOCK: return "socket";
        case S_IFLNK:  return "symlink";
        case S_IFREG:  return "regular";
        case S_IFBLK:  return "block";
        case S_IFDIR:  return "directory";
        case S_IFCHR:  return "character";
        case S_IFIFO:  return "fifo";
        default:       break;
    }
    return "unknown";
}

static const char* family2str(int family) {
    switch (family) {
        case AF_UNIX: return "AF_UNIX";
        case AF_INET: return "AF_INET";
        case AF_INET6: return "AF_INET6";
        default: break;
    }
    return "UNKNOWN";
}

static const char* socktype2str(int socktype) {
    switch (socktype) {
        case SOCK_STREAM: return "SOCK_STREAM";
        case SOCK_DGRAM: return "SOCK_DGRAM";
        case SOCK_RAW: return "SOCK_RAW";
        default: break;
    }
    return "SOCK_RAW";
}

static char* fmtaddr(char *buf, const char *end, unsigned char* addr, int len) {
    while (buf + 2 < end && 0 < len) {
        sprintf(buf, "%02x", *addr);
        buf += 2;
        len -= 1;
        addr += 1;
    }
    return buf;
}

static jstring format_string(JNIEnv *env, struct stat *st, const char *fmt, ...) {
    char details[PATH_MAX];
    va_list va;

    va_start(va, fmt);
    int len = vsnprintf(details, sizeof(details), fmt, va);
    va_end(va);

    // ensure terminated string
    details[sizeof(details) - 1] = '\0';
    return (*env)->NewStringUTF(env, details);
}

JNIEXPORT jstring JNICALL
Java_java_io_FileDescriptor_nativeDescription0(JNIEnv *env, jobject this) {
    FD fd = (*env)->GetIntField(env, this, IO_fd_fdID);

    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
    char link[PATH_MAX];
    int linklen = readlink(fdpath, link, PATH_MAX);
    if (linklen >= 0) {
        link[(unsigned)linklen < PATH_MAX ? linklen : PATH_MAX - 1] = '\0';
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        // return just link value
        return (*env)->NewStringUTF(env, link);
    }

    if ((st.st_mode & S_IFMT) != S_IFSOCK) {
        return format_string(env, &st, "%s: %s", stat2strtype(st.st_mode), link);
    }

    int family;
    socklen_t famlen = sizeof(int);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, &famlen) != 0) {
        return format_string(env, &st, "socket: %s", link);
    }

    int socktype;
    socklen_t typelen = sizeof(int);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &socktype, &typelen) != 0) {
        return format_string(env, &st, "socket: family=%s", family2str(family));
    }

    socketaddress local;
    socklen_t llen = sizeof(socketaddress);
    if (getsockname(fd, &local.sa, &llen) != 0) {
        llen = 0;
    }

    socketaddress remote;
    socklen_t rlen = sizeof(socketaddress);
    if (getpeername(fd, &remote.sa, &rlen) != 0) {
        rlen = 0;
    }

    char details[PATH_MAX];
    int len = snprintf(details, sizeof(details),
            "socket: family=%s type=%s localaddr=", family2str(family), socktype2str(socktype));
    char *end = fmtaddr(details + len, details + sizeof(details), (unsigned char*)&local, llen);
    end += snprintf(end, details + sizeof(details) - end, " remoteaddr=");
    end = fmtaddr(end, details + sizeof(details), (unsigned char*)&remote, rlen);
    // ensure terminated string
    details[sizeof(details) - 1] = '\0';
    return (*env)->NewStringUTF(env, details);
}
