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
#include <stdbool.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

static bool find_sock_details(int sockino, const char* base, bool v6, const char *prefix, char* buf, size_t sz) {
  char filename[16];
  snprintf(filename, sizeof(filename), "/proc/net/%s", base);
  FILE* f = fopen(filename, "r");
  if (!f) {
    return false;
  }
  int r = fscanf(f, "%*[^\n]");
  if (r) {} // suppress warn unused gcc diagnostic

  char la[33], ra[33];
  int lp, rp;
  int ino;
  //   sl  local_address         remote_address        st   tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
  //    0: 0100007F:08AE         00000000:0000         0A   00000000:00000000 00:00000000 00000000  1000        0 2988639
  //  %4d: %08X%08X%08X%08X:%04X %08X%08X%08X%08X:%04X %02X %08X:%08X         %02X:%08lX  %08X       %5u      %8d %d
  bool eof;
  do {
    eof = EOF == fscanf(f, "%*d: %[^:]:%X %[^:]:%X %*X %*X:%*X %*X:%*X %*X %*d %*d %d%*[^\n]\n",
        la, &lp, ra, &rp, &ino);
  } while (ino != sockino && !eof);
  fclose(f);

  if (ino != sockino) {
    return false;
  }

  struct in6_addr a6l, a6r;
  struct in_addr a4l, a4r;
  if (v6) {
    for (int i = 0; i < 4; ++i) {
      sscanf(la + i * 8, "%8" PRIX32, a6l.s6_addr32 + i);
      sscanf(ra + i * 8, "%8" PRIX32, a6r.s6_addr32 + i);
    }
  } else {
    sscanf(la, "%" PRIX32, &a4l.s_addr);
    sscanf(ra, "%" PRIX32, &a4r.s_addr);
  }

  int const af = v6 ? AF_INET6 : AF_INET;
  void* const laddr = v6 ? (void*)&a6l : (void*)&a4l;
  void* const raddr = v6 ? (void*)&a6r : (void*)&a4r;
  char lstrb[48], rstrb[48];
  const char* const lstr = inet_ntop(af, laddr, lstrb, sizeof(lstrb)) ? lstrb : "NONE";
  const char* const rstr = inet_ntop(af, raddr, rstrb, sizeof(rstrb)) ? rstrb : "NONE";
  int msgsz = snprintf(buf, sz, "%s%s localAddr %s localPort %d remoteAddr %s remotePort %d",
        prefix, base, lstr, lp, rstr, rp);
  return msgsz < (int)sz;
}

static const char* sock_details(const char* details, const char *pref, char* buf, size_t sz) {
  int sockino;
  if (sscanf(details, "socket:[%d]", &sockino) <= 0) {
    return details;
  }

  const char* bases[] = { "tcp", "udp", "tcp6", "udp6", NULL };
  for (const char** b = bases; *b; ++b) {
    if (find_sock_details(sockino, *b, 2 <= b - bases, pref, buf, sz)) {
      return buf;
    }
  }

  return details;
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
Java_java_io_FileDescriptor_nativeDescription0(JNIEnv *env, jobject this) {
    FD fd = (*env)->GetIntField(env, this, IO_fd_fdID);

    struct stat st;
    if (fstat(fd, &st) != 0) {
        return (*env)->NewStringUTF(env, "[stat error]");
    }

    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
    char link[PATH_MAX];
    int linklen = readlink(fdpath, link, PATH_MAX);
    if (linklen >= 0) {
        link[(unsigned)linklen < PATH_MAX ? linklen : PATH_MAX - 1] = '\0';
    }

    char details2[PATH_MAX];
    const char *ret = NULL;
    if ((st.st_mode & S_IFMT) == S_IFSOCK) {
        ret = sock_details(link, "socket: ", details2, sizeof(details2));
    } else {
        int len = snprintf(details2, sizeof(details2), "%s: %s", stat2strtype(st.st_mode), link);
        if ((int)sizeof(details2) <= len) {
            details2[sizeof(details2) - 1] = '\0';
        }
        ret = details2;
    }

    return (*env)->NewStringUTF(env, ret);
}
