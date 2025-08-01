/*
 * Copyright (c) 2014, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_JFR_JNI_JFRJNIMETHOD_HPP
#define SHARE_JFR_JNI_JFRJNIMETHOD_HPP

#include "jni.h"

/*
 * Native methods for jdk.jfr.internal.JVM
 */

#ifdef __cplusplus
extern "C" {
#endif

jlong JNICALL jfr_elapsed_counter(JNIEnv* env, jclass jvm);

jboolean JNICALL jfr_create_jfr(JNIEnv* env, jclass jvm, jboolean simulate_failure);

jboolean JNICALL jfr_destroy_jfr(JNIEnv* env, jclass jvm);

void JNICALL jfr_begin_recording(JNIEnv* env, jclass jvm);

jboolean JNICALL jfr_is_recording(JNIEnv* env, jclass jvm);

void JNICALL jfr_end_recording(JNIEnv* env, jclass jvm);

void JNICALL jfr_mark_chunk_final(JNIEnv* env, jclass jvm);

jboolean JNICALL jfr_emit_event(JNIEnv* env, jclass jvm, jlong eventTypeId, jlong timeStamp, jlong when);

jobject JNICALL jfr_get_all_event_classes(JNIEnv* env, jclass jvm);

jlong JNICALL jfr_class_id(JNIEnv* env, jclass jvm, jclass jc);

jstring JNICALL jfr_get_pid(JNIEnv* env, jclass jvm);

jlong JNICALL jfr_stacktrace_id(JNIEnv* env, jclass jvm, jint skip, jlong stack_filter_id);

jlong JNICALL jfr_elapsed_frequency(JNIEnv* env, jclass jvm);

void JNICALL jfr_subscribe_log_level(JNIEnv* env, jclass jvm, jobject log_tag, jint id);

void JNICALL jfr_log(JNIEnv* env, jclass jvm, jint tag_set, jint level, jstring message);

void JNICALL jfr_log_event(JNIEnv* env, jclass jvm, jint level, jobjectArray lines, jboolean system);

void JNICALL jfr_retransform_classes(JNIEnv* env, jclass jvm, jobjectArray classes);

void JNICALL jfr_set_enabled(JNIEnv* env, jclass jvm, jlong event_type_id, jboolean enabled);

void JNICALL jfr_set_file_notification(JNIEnv* env, jclass jvm, jlong delta);

void JNICALL jfr_set_global_buffer_count(JNIEnv* env, jclass jvm, jlong count);

void JNICALL jfr_set_global_buffer_size(JNIEnv* env, jclass jvm, jlong size);

void JNICALL jfr_set_method_sampling_period(JNIEnv* env, jclass jvm, jlong type, jlong periodMillis);

void JNICALL jfr_set_output(JNIEnv* env, jclass jvm, jstring path);

void JNICALL jfr_set_stack_depth(JNIEnv* env, jclass jvm, jint depth);

void JNICALL jfr_set_stacktrace_enabled(JNIEnv* env, jclass jvm, jlong event_type_id, jboolean enabled);

void JNICALL jfr_set_thread_buffer_size(JNIEnv* env, jclass jvm, jlong size);

void JNICALL jfr_set_memory_size(JNIEnv* env, jclass jvm, jlong size);

jboolean JNICALL jfr_set_threshold(JNIEnv* env, jclass jvm, jlong event_type_id, jlong thresholdTicks);

void JNICALL jfr_store_metadata_descriptor(JNIEnv* env, jclass jvm, jbyteArray descriptor);

jlong JNICALL jfr_id_for_thread(JNIEnv* env, jclass jvm, jobject t);

jboolean JNICALL jfr_allow_event_retransforms(JNIEnv* env, jclass jvm);

jboolean JNICALL jfr_is_available(JNIEnv* env, jclass jvm);

jdouble JNICALL jfr_time_conv_factor(JNIEnv* env, jclass jvm);

jlong JNICALL jfr_type_id(JNIEnv* env, jclass jvm, jclass jc);

void JNICALL jfr_set_repository_location(JNIEnv* env, jclass jvm, jstring location);

void JNICALL jfr_set_dump_path(JNIEnv* env, jclass jvm, jstring dumppath);

jstring JNICALL jfr_get_dump_path(JNIEnv* env, jclass jvm);

jobject JNICALL jfr_get_event_writer(JNIEnv* env, jclass jvm);

jobject JNICALL jfr_new_event_writer(JNIEnv* env, jclass jvm);

void JNICALL jfr_event_writer_flush(JNIEnv* env, jclass jvm, jobject writer, jint used_size, jint requested_size);

jlong JNICALL jfr_commit(JNIEnv* env, jclass cls, jlong next_position);
void JNICALL jfr_flush(JNIEnv* env, jclass jvm);
void JNICALL jfr_abort(JNIEnv* env, jclass jvm, jstring errorMsg);

jboolean JNICALL jfr_add_string_constant(JNIEnv* env, jclass jvm, jlong id, jstring string);

void JNICALL jfr_uncaught_exception(JNIEnv* env, jclass jvm, jobject thread, jthrowable throwable);

void JNICALL jfr_set_force_instrumentation(JNIEnv* env, jclass jvm, jboolean force);

jlong JNICALL jfr_get_unloaded_event_classes_count(JNIEnv* env, jclass jvm);

jboolean JNICALL jfr_set_throttle(JNIEnv* env, jclass jvm, jlong event_type_id, jlong event_sample_size, jlong period_ms);

void JNICALL jfr_set_cpu_throttle(JNIEnv* env, jclass jvm, jdouble rate, jboolean auto_adapt);

void JNICALL jfr_set_miscellaneous(JNIEnv* env, jclass jvm, jlong id, jlong value);

void JNICALL jfr_emit_old_object_samples(JNIEnv* env, jclass jvm, jlong cutoff_ticks, jboolean, jboolean);

jboolean JNICALL jfr_should_rotate_disk(JNIEnv* env, jclass jvm);

void JNICALL jfr_exclude_thread(JNIEnv* env, jclass jvm, jobject t);

void JNICALL jfr_include_thread(JNIEnv* env, jclass jvm, jobject t);

jboolean JNICALL jfr_is_thread_excluded(JNIEnv* env, jclass jvm, jobject t);

jlong JNICALL jfr_chunk_start_nanos(JNIEnv* env, jclass jvm);

jobject JNICALL jfr_get_configuration(JNIEnv* env, jclass jvm, jobject clazz);

jboolean JNICALL jfr_set_configuration(JNIEnv* env, jclass jvm, jobject clazz, jobject configuration);

jlong JNICALL jfr_get_type_id_from_string(JNIEnv* env, jclass jvm, jstring type);

jboolean JNICALL jfr_is_class_excluded(JNIEnv* env, jclass jvm, jclass clazz);

jboolean JNICALL jfr_is_class_instrumented(JNIEnv* env, jclass jvm, jclass clazz);

jboolean JNICALL jfr_is_containerized(JNIEnv* env, jclass jvm);

jlong JNICALL jfr_host_total_memory(JNIEnv* env, jclass jvm);

jlong JNICALL jfr_host_total_swap_memory(JNIEnv* env, jclass jvm);

void JNICALL jfr_emit_data_loss(JNIEnv* env, jclass jvm, jlong bytes);

jlong JNICALL jfr_register_stack_filter(JNIEnv* env, jclass jvm, jobjectArray classes, jobjectArray methods);

void JNICALL jfr_unregister_stack_filter(JNIEnv* env, jclass jvm, jlong id);

jlong JNICALL jfr_nanos_now(JNIEnv* env, jclass jvm);

void JNICALL jfr_start_after_restore(JNIEnv* env, jclass jvm);

jboolean JNICALL jfr_is_product(JNIEnv* env, jclass jvm);

jlongArray JNICALL jfr_set_method_trace_filters(JNIEnv* env, jclass jvm, jobjectArray classes, jobjectArray methods, jobjectArray annotations, jintArray modifications);

jlongArray JNICALL jfr_drain_stale_method_tracer_ids(JNIEnv* env, jclass);

#ifdef __cplusplus
}
#endif

#endif // SHARE_JFR_JNI_JFRJNIMETHOD_HPP
