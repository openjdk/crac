/*
 * Copyright (c) 2016, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_JFR_JNI_JFRUPCALLS_HPP
#define SHARE_JFR_JNI_JFRUPCALLS_HPP

#include "jfr/utilities/jfrAllocation.hpp"
#include "jfr/utilities/jfrTypes.hpp"
#include "jni.h"
#include "utilities/exceptions.hpp"

class ClassFileStream;
class InstanceKlass;
class JfrTracedMethod;
class JavaThread;
template <typename E> class GrowableArray;

//
// Upcalls to Java for instrumentation purposes.
// Targets are located in jdk.jfr.internal.JVMUpcalls.
//
class JfrUpcalls : AllStatic {
 public:
  static void new_bytes_eager_instrumentation(jlong trace_id,
                                              jboolean force_instrumentation,
                                              jboolean boot_class_loader,
                                              jclass super,
                                              jint class_data_len,
                                              const unsigned char* class_data,
                                              jint* new_class_data_len,
                                              unsigned char** new_class_data,
                                              TRAPS);

  static void on_retransform(jlong trace_id,
                             jclass class_being_redefined,
                             jint class_data_len,
                             const unsigned char* class_data,
                             jint* new_class_data_len,
                             unsigned char** new_class_data,
                             TRAPS);

  // Caller needs ResourceMark
  static ClassFileStream* on_method_trace(InstanceKlass* ik, const ClassFileStream* stream,
                                          GrowableArray<JfrTracedMethod>* methods,
                                          TRAPS);

  static void publish_method_timers_for_klass(traceid klass_id, TRAPS);

  static bool unhide_internal_types(TRAPS);
  static void request_start_after_restore();
};

#endif // SHARE_JFR_JNI_JFRUPCALLS_HPP
