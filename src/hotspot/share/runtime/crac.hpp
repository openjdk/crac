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
#include "runtime/cracStackDumpParser.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/handles.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/heapDumpParser.hpp"

// xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
#define UUID_LENGTH 36

class crac: AllStatic {
public:
  // Returns true if using the experimetnal portable mode.
  static bool is_portable_mode();

  static void vm_create_start();
  static bool prepare_checkpoint();
  static Handle checkpoint(jarray fd_arr, jobjectArray obj_arr, bool dry_run, jlong jcmd_stream, TRAPS);
  // Restore in the classic mode.
  static void restore();

  static jlong restore_start_time();
  static jlong uptime_since_restore();

  static void record_time_before_checkpoint();
  static void update_javaTimeNanos_offset();

  static jlong monotonic_time_offset() {
    return javaTimeNanos_offset;
  }

  static void initialize_time_counters();

  // Portable mode

  // Restores classes and objects.
  static void restore_heap(TRAPS);
  // Restores thread states and launches their execution. Should only be called
  // once, after restore_heap() has been called.
  static void restore_threads(TRAPS);
  // Called by RestoreStub to prepare information about frames to restore.
  static Deoptimization::UnrollBlock *fetch_frame_info(JavaThread *current);
  // Called by RestoreStub to fill in the skeletal frames just created.
  static void fill_in_frames(JavaThread *current);

private:
  static bool read_bootid(char *dest);

  static jlong checkpoint_millis;
  static jlong checkpoint_nanos;
  static char checkpoint_bootid[UUID_LENGTH];
  static jlong javaTimeNanos_offset;

  static JavaValue restore_current_thread(TRAPS);
  static void clear_restoration_data();

  static ParsedHeapDump  *_heap_dump;
  static ParsedStackDump *_stack_dump;
  static HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> *_portable_restored_classes;
  static HeapDumpTable<jobject, AnyObj::C_HEAP> *_portable_restored_objects;
};

#endif //SHARE_RUNTIME_CRAC_HPP
