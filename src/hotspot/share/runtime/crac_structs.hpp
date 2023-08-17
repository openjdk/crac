/*
 * Copyright (c) 2023, Azul Systems, Inc. All rights reserved.
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
 */

#ifndef SHARE_RUNTIME_CRAC_STRUCTS_HPP
#define SHARE_RUNTIME_CRAC_STRUCTS_HPP

#include "jvm.h"
#include "runtime/arguments.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "runtime/vmOperation.hpp"

#ifdef LINUX
#include "attachListener_linux.hpp"
#include "linuxAttachOperation.hpp"
#include "services/attachListener.hpp"
#endif

struct CracFailDep {
  int _type;
  char* _msg;
  CracFailDep(int type, char* msg) :
    _type(type),
    _msg(msg)
  { }
  CracFailDep() :
    _type(JVM_CR_FAIL),
    _msg(NULL)
  { }
};

struct CracRestoreParameters : public StackObj {
  jlong restore_time;
  jlong restore_nanos;
  GrowableArray<const char *> flags;
  GrowableArray<const char *> properties;
  GrowableArray<const char *> args;
  GrowableArray<const char *> envs;

  CracRestoreParameters();

  // Write parameters into fd.
  // Return true if successful.
  bool serialize(int fd) const;

  // Read parameters from fd.
  // Return true if successful.
  bool deserialize(int fd);
};

class VM_Crac: public VM_Operation {
  jarray _fd_arr;
  const bool _dry_run;
  bool _ok;
  GrowableArray<CracFailDep>* _failures;
  CracRestoreParameters _restore_parameters;
  outputStream* _ostream;
#ifdef LINUX
  LinuxAttachOperation* _attach_op;
#endif //LINUX

public:
  VM_Crac(jarray fd_arr, jobjectArray obj_arr, bool dry_run, bufferedStream* jcmd_stream) :
    _fd_arr(fd_arr),
    _dry_run(dry_run),
    _ok(false),
    _failures(new (mtInternal) GrowableArray<CracFailDep>(0, mtInternal)),
    _restore_parameters(),
    _ostream(jcmd_stream ? jcmd_stream : tty)
#ifdef LINUX
    , _attach_op(jcmd_stream ? LinuxAttachListener::get_current_op() : NULL)
#endif //LINUX
  { }

  ~VM_Crac() {
    delete _failures;
  }

  GrowableArray<CracFailDep>* failures() { return _failures; }
  bool ok() { return _ok; }
  GrowableArray<const char *>* new_args() { return &_restore_parameters.args; }
  GrowableArray<const char *>* new_properties() { return &_restore_parameters.properties; }
  virtual bool allow_nested_vm_operations() const  { return true; }
  VMOp_Type type() const { return VMOp_VM_Crac; }
  void doit();
  bool read_shm(int shmid);

private:
  bool is_claimed_fd(int fd);
  bool is_socket_from_jcmd(int sock_fd);
  void report_ok_to_jcmd_if_any();
  void print_resources(const char* msg, ...);
  void trace_cr(const char* msg, ...);
  bool check_fds();
  bool memory_checkpoint();
  void memory_restore();
};

class CracSHM {
  char _path[128];
public:
  CracSHM(int id) {
    int shmpathlen = snprintf(_path, sizeof(_path), "/crac_%d", id);
    if (shmpathlen < 0 || sizeof(_path) <= (size_t)shmpathlen) {
      fprintf(stderr, "shmpath is too long: %d\n", shmpathlen);
    }
  }

  int open(int mode);
  void unlink();
};

#endif //SHARE_RUNTIME_CRAC_STRUCTS_HPP
