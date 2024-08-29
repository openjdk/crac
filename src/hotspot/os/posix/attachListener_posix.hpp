/*
 * Copyright (c) 2005, 2022, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2022, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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
 *
 */

#ifndef OS_POSIX_ATTACHLISTENER_POSIX_HPP
#define OS_POSIX_ATTACHLISTENER_POSIX_HPP

class PosixAttachListener;

#if INCLUDE_SERVICES

#include "posixAttachOperation.hpp"
#include "services/attachListener.hpp"

#include <sys/un.h>

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   sizeof(((struct sockaddr_un *)0)->sun_path)
#endif

class PosixAttachListener: AllStatic {
 private:
  // the path to which we bind the UNIX domain socket
  static char _path[UNIX_PATH_MAX];
  static bool _has_path;

  // the file descriptor for the listening socket
  static volatile int _listener;

  static bool _atexit_registered;

  // this is for proper reporting JDK.Chekpoint processing to jcmd peer
  static PosixAttachOperation* _current_op;

  // reads a request from the given connected socket
  static PosixAttachOperation* read_request(int s);

 public:
  enum {
    ATTACH_PROTOCOL_VER = 1                     // protocol version
  };
  enum {
    ATTACH_ERROR_BADVERSION     = 101           // error codes
  };

  static void set_path(char* path) {
    if (path == nullptr) {
      _path[0] = '\0';
      _has_path = false;
    } else {
      strncpy(_path, path, UNIX_PATH_MAX);
      _path[UNIX_PATH_MAX-1] = '\0';
      _has_path = true;
    }
  }

  static void set_listener(int s)               { _listener = s; }

  // initialize the listener, returns 0 if okay
  static int init();

  static char* path()                   { return _path; }
  static bool has_path()                { return _has_path; }
  static int listener()                 { return _listener; }

  // write the given buffer to a socket
  static int write_fully(int s, char* buf, size_t len);

  static PosixAttachOperation* dequeue();
  static PosixAttachOperation* get_current_op();
  static void reset_current_op();

};

#endif // INCLUDE_SERVICES

#endif // OS_POSIX_ATTACHLISTENER_POSIX_HPP
