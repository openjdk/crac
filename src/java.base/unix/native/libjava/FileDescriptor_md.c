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

JNIEXPORT jstring JNICALL
Java_java_io_FileDescriptor_getPath(JNIEnv *env, jobject obj) {
    int fd = (*env)->GetIntField(env, obj, IO_fd_fdID);
    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
    char link[PATH_MAX];
    int ret = readlink(fdpath, link, PATH_MAX);
    if (ret >= 0) {
        link[(unsigned)ret < PATH_MAX ? ret : PATH_MAX - 1] = '\0';
        return (*env)->NewStringUTF(env, link);
    }
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_java_io_FileDescriptor_getType(JNIEnv *env, jobject obj) {
    int fd = (*env)->GetIntField(env, obj, IO_fd_fdID);
    struct stat st;
    if (fstat(fd, &st) == 0) {
        return (*env)->NewStringUTF(env, stat2strtype(st.st_mode));
    } else {
        return NULL;
    }
}

JNIEXPORT jlong JNICALL
Java_java_io_FileDescriptor_getOffset(JNIEnv *env, jobject obj) {
    int fd = (*env)->GetIntField(env, obj, IO_fd_fdID);
    jlong offset = lseek(fd, 0, SEEK_CUR);
    if (offset < 0) {
        if (errno == ESPIPE) {
            return 0;
        } else {
            perror("CRaC: cannot find file descriptor offset");
            return offset;
        }
    }
    return offset;
}

JNIEXPORT jint JNICALL
Java_java_io_FileDescriptor_getFlags(JNIEnv *env, jobject obj) {
    int fd = (*env)->GetIntField(env, obj, IO_fd_fdID);
    return fcntl(fd, F_GETFL);
}

JNIEXPORT jboolean JNICALL
Java_java_io_FileDescriptor_reopen(JNIEnv *env, jobject obj, jint fd, jstring path, jint flags, jlong offset) {
    if (fcntl(fd, F_GETFD) != -1) {
        JNU_ThrowByName(env, "jdk/crac/impl/CheckpointOpenFileException", "File descriptor is already open");
    }
    // assert errno is EBADF?
    jboolean copy;
    const char *cpath = (*env)->GetStringUTFChars(env, path, &copy);
    int firstFd = open(cpath, flags);
    (*env)->ReleaseStringUTFChars(env, path, cpath);
    jboolean result = JNI_TRUE;
    if (firstFd < 0) {
        perror("CRaC: Failed to reopen file descriptor");
        return JNI_FALSE;
    } else if (firstFd != fd) {
        if (dup2(firstFd, fd) < 0) {
            perror("CRaC: Failed to dup2 new file descriptor to original one");
            result = JNI_FALSE;
        }
        if (close(firstFd) < 0) {
            perror("CRaC: failed to close opened file descriptor");
        }
    }
    if (result) {
        if ((offset > 0 && lseek(fd, offset, SEEK_SET) < 0) ||
            (offset < 0 && lseek(fd, 0, SEEK_END) < 0)) {
            perror("CRaC: Failed to lseek reopened file descriptor");
            close(fd);
            return JNI_FALSE;
        }
    }
    return result;
}

JNIEXPORT jboolean JNICALL
Java_java_io_FileDescriptor_reopenNull(JNIEnv *env, jobject obj, jint fd) {
    if (fcntl(fd, F_GETFD) != -1) {
        JNU_ThrowByName(env, "jdk/crac/impl/CheckpointOpenFileException", "File descriptor is already open");
    }
    // assert errno is EBADF?
    int firstFd = open("/dev/null", O_WRONLY);
    if (firstFd < 0) {
        perror("CRaC: Failed to reopen file descriptor using /dev/null");
        return JNI_FALSE;
    } else if (firstFd == fd) {
        return JNI_TRUE;
    }
    jboolean result = JNI_TRUE;
    if (dup2(firstFd, fd) < 0) {
        perror("CRaC: Failed to dup2 new file descriptor to original one");
        result = JNI_FALSE;
    }
    if (close(firstFd) < 0) {
        perror("CRaC: failed to close opened file descriptor");
    }
    return result;
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
