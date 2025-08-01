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

#include "jfr/jni/jfrJniMethod.hpp"
#include "jfr/jni/jfrJniMethodRegistration.hpp"
#include "logging/log.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.inline.hpp"
#include "utilities/exceptions.hpp"

JfrJniMethodRegistration::JfrJniMethodRegistration(JNIEnv* env) {
  assert(env != nullptr, "invariant");
  jclass jfr_clz = env->FindClass("jdk/jfr/internal/JVM");
  if (jfr_clz != nullptr) {
    JNINativeMethod method[] = {
      (char*)"beginRecording", (char*)"()V", (void*)jfr_begin_recording,
      (char*)"isRecording", (char*)"()Z", (void*)jfr_is_recording,
      (char*)"endRecording", (char*)"()V", (void*)jfr_end_recording,
      (char*)"markChunkFinal", (char*)"()V", (void*)jfr_mark_chunk_final,
      (char*)"counterTime", (char*)"()J", (void*)jfr_elapsed_counter,
      (char*)"createJFR", (char*)"(Z)Z", (void*)jfr_create_jfr,
      (char*)"destroyJFR", (char*)"()Z", (void*)jfr_destroy_jfr,
      (char*)"emitEvent", (char*)"(JJJ)Z", (void*)jfr_emit_event,
      (char*)"getAllEventClasses", (char*)"()Ljava/util/List;", (void*)jfr_get_all_event_classes,
      (char*)"getClassId", (char*)"(Ljava/lang/Class;)J", (void*)jfr_class_id,
      (char*)"getPid", (char*)"()Ljava/lang/String;", (void*)jfr_get_pid,
      (char*)"getStackTraceId", (char*)"(IJ)J", (void*)jfr_stacktrace_id,
      (char*)"getThreadId", (char*)"(Ljava/lang/Thread;)J", (void*)jfr_id_for_thread,
      (char*)"getTicksFrequency", (char*)"()J", (void*)jfr_elapsed_frequency,
      (char*)"subscribeLogLevel", (char*)"(Ljdk/jfr/internal/LogTag;I)V", (void*)jfr_subscribe_log_level,
      (char*)"log", (char*)"(IILjava/lang/String;)V", (void*)jfr_log,
      (char*)"logEvent", (char*)"(I[Ljava/lang/String;Z)V", (void*)jfr_log_event,
      (char*)"retransformClasses", (char*)"([Ljava/lang/Class;)V", (void*)jfr_retransform_classes,
      (char*)"setEnabled", (char*)"(JZ)V", (void*)jfr_set_enabled,
      (char*)"setFileNotification", (char*)"(J)V", (void*)jfr_set_file_notification,
      (char*)"setGlobalBufferCount", (char*)"(J)V", (void*)jfr_set_global_buffer_count,
      (char*)"setGlobalBufferSize", (char*)"(J)V", (void*)jfr_set_global_buffer_size,
      (char*)"setMethodSamplingPeriod", (char*)"(JJ)V", (void*)jfr_set_method_sampling_period,
      (char*)"setOutput", (char*)"(Ljava/lang/String;)V", (void*)jfr_set_output,
      (char*)"setStackDepth", (char*)"(I)V", (void*)jfr_set_stack_depth,
      (char*)"setStackTraceEnabled", (char*)"(JZ)V", (void*)jfr_set_stacktrace_enabled,
      (char*)"setThreadBufferSize", (char*)"(J)V", (void*)jfr_set_thread_buffer_size,
      (char*)"setMemorySize", (char*)"(J)V", (void*)jfr_set_memory_size,
      (char*)"setThreshold", (char*)"(JJ)Z", (void*)jfr_set_threshold,
      (char*)"storeMetadataDescriptor", (char*)"([B)V", (void*)jfr_store_metadata_descriptor,
      (char*)"getAllowedToDoEventRetransforms", (char*)"()Z", (void*)jfr_allow_event_retransforms,
      (char*)"isAvailable", (char*)"()Z", (void*)jfr_is_available,
      (char*)"getTimeConversionFactor", (char*)"()D", (void*)jfr_time_conv_factor,
      (char*)"getTypeId", (char*)"(Ljava/lang/Class;)J", (void*)jfr_type_id,
      (char*)"getEventWriter", (char*)"()Ljdk/jfr/internal/event/EventWriter;", (void*)jfr_get_event_writer,
      (char*)"newEventWriter", (char*)"()Ljdk/jfr/internal/event/EventWriter;", (void*)jfr_new_event_writer,
      (char*)"flush", (char*)"(Ljdk/jfr/internal/event/EventWriter;II)V", (void*)jfr_event_writer_flush,
      (char*)"commit", (char*)"(J)J", (void*)jfr_commit,
      (char*)"flush", (char*)"()V", (void*)jfr_flush,
      (char*)"setRepositoryLocation", (char*)"(Ljava/lang/String;)V", (void*)jfr_set_repository_location,
      (char*)"setDumpPath", (char*)"(Ljava/lang/String;)V", (void*)jfr_set_dump_path,
      (char*)"getDumpPath", (char*)"()Ljava/lang/String;", (void*)jfr_get_dump_path,
      (char*)"abort", (char*)"(Ljava/lang/String;)V", (void*)jfr_abort,
      (char*)"addStringConstant", (char*)"(JLjava/lang/String;)Z", (void*)jfr_add_string_constant,
      (char*)"uncaughtException", (char*)"(Ljava/lang/Thread;Ljava/lang/Throwable;)V", (void*)jfr_uncaught_exception,
      (char*)"setForceInstrumentation", (char*)"(Z)V", (void*)jfr_set_force_instrumentation,
      (char*)"getUnloadedEventClassCount", (char*)"()J", (void*)jfr_get_unloaded_event_classes_count,
      (char*)"setMiscellaneous", (char*)"(JJ)V", (void*)jfr_set_miscellaneous,
      (char*)"setThrottle", (char*)"(JJJ)Z", (void*)jfr_set_throttle,
      (char*)"setCPUThrottle", (char*)"(DZ)V", (void*)jfr_set_cpu_throttle,
      (char*)"emitOldObjectSamples", (char*)"(JZZ)V", (void*)jfr_emit_old_object_samples,
      (char*)"shouldRotateDisk", (char*)"()Z", (void*)jfr_should_rotate_disk,
      (char*)"exclude", (char*)"(Ljava/lang/Thread;)V", (void*)jfr_exclude_thread,
      (char*)"include", (char*)"(Ljava/lang/Thread;)V", (void*)jfr_include_thread,
      (char*)"isExcluded", (char*)"(Ljava/lang/Thread;)Z", (void*)jfr_is_thread_excluded,
      (char*)"getChunkStartNanos", (char*)"()J", (void*)jfr_chunk_start_nanos,
      (char*)"getConfiguration", (char*)"(Ljava/lang/Class;)Ljava/lang/Object;", (void*)jfr_get_configuration,
      (char*)"setConfiguration", (char*)"(Ljava/lang/Class;Ljdk/jfr/internal/event/EventConfiguration;)Z", (void*)jfr_set_configuration,
      (char*)"getTypeId", (char*)"(Ljava/lang/String;)J", (void*)jfr_get_type_id_from_string,
      (char*)"isExcluded", (char*)"(Ljava/lang/Class;)Z", (void*)jfr_is_class_excluded,
      (char*)"isInstrumented", (char*)"(Ljava/lang/Class;)Z", (void*) jfr_is_class_instrumented,
      (char*)"isContainerized", (char*)"()Z", (void*) jfr_is_containerized,
      (char*)"hostTotalMemory", (char*)"()J", (void*) jfr_host_total_memory,
      (char*)"hostTotalSwapMemory", (char*)"()J", (void*) jfr_host_total_swap_memory,
      (char*)"emitDataLoss", (char*)"(J)V", (void*)jfr_emit_data_loss,
      (char*)"registerStackFilter", (char*)"([Ljava/lang/String;[Ljava/lang/String;)J", (void*)jfr_register_stack_filter,
      (char*)"unregisterStackFilter", (char*)"(J)V", (void*)jfr_unregister_stack_filter,
      (char*)"nanosNow", (char*)"()J", (void*)jfr_nanos_now,
      (char*)"startFlightRecorderAfterRestore", (char*)"()V", (void*)jfr_start_after_restore,
      (char*)"isProduct", (char*)"()Z", (void*)jfr_is_product,
      (char*)"setMethodTraceFilters", (char*)"([Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[I)[J", (void*)jfr_set_method_trace_filters,
      (char*)"drainStaleMethodTracerIds", (char*)"()[J", (void*)jfr_drain_stale_method_tracer_ids
    };

    const size_t method_array_length = sizeof(method) / sizeof(JNINativeMethod);
    if (env->RegisterNatives(jfr_clz, method, (jint)method_array_length) != JNI_OK) {
      JavaThread* jt = JavaThread::thread_from_jni_environment(env);
      assert(jt != nullptr, "invariant");
      assert(jt->thread_state() == _thread_in_native, "invariant");
      ThreadInVMfromNative transition(jt);
      log_error(jfr, system)("RegisterNatives for JVM class failed!");
    }
    env->DeleteLocalRef(jfr_clz);
  }
}

