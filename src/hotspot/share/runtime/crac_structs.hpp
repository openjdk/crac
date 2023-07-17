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

class CracRestoreParameters : public CHeapObj<mtInternal> {
  char* _raw_content;
  GrowableArray<const char *>* _properties;
  const char* _args;

  struct header {
    jlong _restore_time;
    jlong _restore_nanos;
    int _nflags;
    int _nprops;
    int _env_memory_size;
  };

  static bool write_check_error(int fd, const void *buf, int count) {
    int wret = write(fd, buf, count);
    if (wret != count) {
      if (wret < 0) {
        perror("shm error");
      } else {
        fprintf(stderr, "write shm truncated");
      }
      return false;
    }
    return true;
  }

  static int system_props_length(const SystemProperty* props) {
    int len = 0;
    while (props != NULL) {
      ++len;
      props = props->next();
    }
    return len;
  }

  static int env_vars_size(const char* const * env) {
    int len = 0;
    for (; *env; ++env) {
      len += (int)strlen(*env) + 1;
    }
    return len;
  }

 public:
  const char *args() const { return _args; }
  GrowableArray<const char *>* properties() const { return _properties; }

  CracRestoreParameters() :
    _raw_content(NULL),
    _properties(new (ResourceObj::C_HEAP, mtInternal) GrowableArray<const char *>(0, mtInternal)),
    _args(NULL)
  {}

  ~CracRestoreParameters() {
    if (_raw_content) {
      FREE_C_HEAP_ARRAY(char, _raw_content);
    }
    delete _properties;
  }

  static bool write_to(int fd,
      const char* const* flags, int num_flags,
      const SystemProperty* props,
      const char *args,
      jlong restore_time,
      jlong restore_nanos) {
    header hdr = {
      restore_time,
      restore_nanos,
      num_flags,
      system_props_length(props),
      env_vars_size(os::get_environ())
    };

    if (!write_check_error(fd, (void *)&hdr, sizeof(header))) {
      return false;
    }

    for (int i = 0; i < num_flags; ++i) {
      if (!write_check_error(fd, flags[i], (int)strlen(flags[i]) + 1)) {
        return false;
      }
    }

    const SystemProperty* p = props;
    while (p != NULL) {
      char prop[4096];
      int len = snprintf(prop, sizeof(prop), "%s=%s", p->key(), p->value());
      guarantee((0 < len) && ((unsigned)len < sizeof(prop)), "property does not fit temp buffer");
      if (!write_check_error(fd, prop, len+1)) {
        return false;
      }
      p = p->next();
    }

    // Write env vars
    for (char** env = os::get_environ(); *env; ++env) {
      if (!write_check_error(fd, *env, (int)strlen(*env) + 1)) {
        return false;
      }
    }

    return write_check_error(fd, args, (int)strlen(args)+1); // +1 for null char
  }

  bool read_from(int fd);

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
    _failures(new (ResourceObj::C_HEAP, mtInternal) GrowableArray<CracFailDep>(0, mtInternal)),
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
  const char* new_args() { return _restore_parameters.args(); }
  GrowableArray<const char *>* new_properties() { return _restore_parameters.properties(); }
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
