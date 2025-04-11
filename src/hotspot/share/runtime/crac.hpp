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

#ifndef SHARE_RUNTIME_CRAC_HPP
#define SHARE_RUNTIME_CRAC_HPP

#include "memory/allStatic.hpp"
#include "runtime/crac_engine.hpp"
#include "runtime/handles.hpp"
#include "utilities/exceptions.hpp"

// xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
#define UUID_LENGTH 36

class crac: AllStatic {
  friend class VM_Crac;
public:
  static void print_engine_info_and_exit();
  static void vm_create_start();

  static bool prepare_checkpoint();
  static Handle checkpoint(jarray fd_arr, jobjectArray obj_arr, bool dry_run, jlong jcmd_stream, TRAPS);

  struct crac_restore_data {
    jlong restore_time;
    jlong restore_nanos;
  };
  static void prepare_restore(crac_restore_data& restore_data);
  static void restore(crac_restore_data& restore_data);

  static jlong restore_start_time();
  static jlong uptime_since_restore();

  static jlong monotonic_time_offset() {
    return javaTimeNanos_offset;
  }

  static void reset_time_counters();

private:
  static jlong checkpoint_wallclock_seconds;
  static jlong checkpoint_wallclock_nanos;
  static jlong checkpoint_monotonic_nanos;
  static char checkpoint_bootid[UUID_LENGTH];
  static jlong javaTimeNanos_offset;

  static CracEngine *_engine;

  static bool read_bootid(char *dest);

  static void record_time_before_checkpoint();
  static void update_javaTimeNanos_offset();

  static int checkpoint_restore(int *shmid);
};

#endif //SHARE_RUNTIME_CRAC_HPP
