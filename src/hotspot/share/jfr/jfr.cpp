/*
 * Copyright (c) 2019, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "jfr/instrumentation/jfrEventClassTransformer.hpp"
#include "jfr/jfr.hpp"
#include "jfr/jni/jfrJavaSupport.hpp"
#include "jfr/jni/jfrUpcalls.hpp"
#include "jfr/leakprofiler/leakProfiler.hpp"
#include "jfr/periodic/jfrOSInterface.hpp"
#include "jfr/recorder/jfrRecorder.hpp"
#include "jfr/recorder/checkpoint/jfrCheckpointManager.hpp"
#include "jfr/recorder/repository/jfrEmergencyDump.hpp"
#include "jfr/recorder/service/jfrOptionSet.hpp"
#include "jfr/recorder/service/jfrOptionSet.hpp"
#include "jfr/recorder/repository/jfrRepository.hpp"
#include "jfr/support/jfrKlassExtension.hpp"
#include "jfr/support/jfrResolution.hpp"
#include "jfr/support/jfrThreadLocal.hpp"
#include "jfr/support/methodtracer/jfrMethodTracer.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "oops/klass.hpp"
#include "runtime/flags/jvmFlag.hpp"
#include "runtime/java.hpp"
#include "runtime/javaThread.hpp"

bool Jfr::is_enabled() {
  return JfrRecorder::is_enabled();
}

bool Jfr::is_disabled() {
  return JfrRecorder::is_disabled();
}

bool Jfr::is_recording() {
  return JfrRecorder::is_recording();
}

void Jfr::on_create_vm_1() {
  if (!JfrRecorder::on_create_vm_1()) {
    vm_exit_during_initialization("Failure when starting JFR on_create_vm_1");
  }
}

void Jfr::on_create_vm_2() {
  if (!JfrRecorder::on_create_vm_2()) {
    vm_exit_during_initialization("Failure when starting JFR on_create_vm_2");
  }
}

void Jfr::on_create_vm_3() {
  if (!JfrRecorder::on_create_vm_3()) {
    vm_exit_during_initialization("Failure when starting JFR on_create_vm_3");
  }
}

void Jfr::on_unloading_classes() {
  if (JfrRecorder::is_created() || JfrRecorder::is_started_on_commandline()) {
    JfrCheckpointManager::on_unloading_classes();
  }
}

void Jfr::on_klass_creation(InstanceKlass*& ik, ClassFileParser& parser, TRAPS) {
  if (IS_EVENT_OR_HOST_KLASS(ik)) {
    JfrEventClassTransformer::on_klass_creation(ik, parser, THREAD);
    return;
  }
  if (JfrMethodTracer::in_use()) {
    JfrMethodTracer::on_klass_creation(ik, parser, THREAD);
  }
}

void Jfr::on_klass_redefinition(const InstanceKlass* ik, Thread* thread) {
  assert(JfrMethodTracer::in_use(), "invariant");
  JfrMethodTracer::on_klass_redefinition(ik, thread);
}


bool Jfr::is_excluded(Thread* t) {
  return JfrJavaSupport::is_excluded(t);
}

void Jfr::include_thread(Thread* t) {
  JfrJavaSupport::include(t);
}

void Jfr::exclude_thread(Thread* t) {
  JfrJavaSupport::exclude(t);
}

void Jfr::on_thread_start(Thread* t) {
  JfrThreadLocal::on_start(t);
}

void Jfr::on_thread_exit(Thread* t) {
  JfrThreadLocal::on_exit(t);
}

void Jfr::on_java_thread_start(JavaThread* starter, JavaThread* startee) {
  JfrThreadLocal::on_java_thread_start(starter, startee);
}

void Jfr::on_set_current_thread(JavaThread* jt, oop thread) {
  JfrThreadLocal::on_set_current_thread(jt, thread);
}

void Jfr::initialize_main_thread(JavaThread* jt) {
  JfrThreadLocal::initialize_main_thread(jt);
}

void Jfr::on_resolution(const CallInfo& info, TRAPS) {
  JfrResolution::on_runtime_resolution(info, THREAD);
}

void Jfr::on_backpatching(const Method* callee_method, JavaThread* jt) {
  JfrResolution::on_backpatching(callee_method, jt);
}

#ifdef COMPILER1
void Jfr::on_resolution(const GraphBuilder* builder, const ciKlass* holder, const ciMethod* target) {
  JfrResolution::on_c1_resolution(builder, holder, target);
}
#endif

#ifdef COMPILER2
void Jfr::on_resolution(const Parse* parse, const ciKlass* holder, const ciMethod* target) {
  JfrResolution::on_c2_resolution(parse, holder, target);
}
#endif

#if INCLUDE_JVMCI
void Jfr::on_resolution(const Method* caller, const Method* target, TRAPS) {
  JfrResolution::on_jvmci_resolution(caller, target, CHECK);
}
#endif

void Jfr::on_vm_shutdown(bool exception_handler, bool halt) {
  if (!halt && JfrRecorder::is_recording()) {
    JfrEmergencyDump::on_vm_shutdown(exception_handler);
  }
}

void Jfr::on_vm_error_report(outputStream* st) {
  if (JfrRecorder::is_recording()) {
    JfrRepository::on_vm_error_report(st);
  }
}

bool Jfr::on_flight_recorder_option(const JavaVMOption** option, char* delimiter) {
  return JfrOptionSet::parse_flight_recorder_option(option, delimiter);
}

bool Jfr::on_start_flight_recording_option(const JavaVMOption** option, char* delimiter) {
  return JfrOptionSet::parse_start_flight_recording_option(option, delimiter);
}

void Jfr::before_checkpoint() {
  JfrOSInterface::before_checkpoint();
}

void Jfr::after_restore() {
  const char *jfr_flag = "StartFlightRecording";
  JVMFlag *flag = JVMFlag::find_flag(jfr_flag);
  if (flag->get_origin() == JVMFlagOrigin::CRAC_RESTORE) {
    // -XX:StartFlightRecording passed on restore
    assert(JfrOptionSet::start_flight_recording_options() == nullptr, "should have been released");
    size_t buf_len = 4 + strlen(jfr_flag) + 1 + strlen(flag->get_ccstr()) + 1;
    ResourceMark rm;
    char *buf = NEW_RESOURCE_ARRAY(char, buf_len);
    snprintf(buf, buf_len, "-XX:%s=%s", jfr_flag, flag->get_ccstr());
    JavaVMOption option;
    option.optionString = buf;
    option.extraInfo = nullptr;
    const JavaVMOption *option_ptr = &option;
    JfrOptionSet::parse_start_flight_recording_option(&option_ptr, buf + 4 + strlen(jfr_flag));
    // We cannot invoke this directly now as DCmdStart command would be blocked
    // trying to register new file descriptors. Instead we just record a request and
    // the recording will be started at the right moment from JDKResource.
    JfrUpcalls::request_start_after_restore();
  }
}
