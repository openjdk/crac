/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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

#include "runtime/cracRecompiler.hpp"
#include "code/nmethod.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/compileTask.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/resourceArea.hpp"
#include "nmt/memTag.hpp"
#include "oops/klass.inline.hpp"
#include "oops/metadata.hpp"
#include "oops/method.hpp"
#include "runtime/handles.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/mutex.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/thread.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"

// Records information about a decompiled method.
//
// Safepoints may occur between the moments when it is recorded and used which
// can lead to the Method* getting freed. To fight this we use the same
// mechanisms as CompileTask does:
// 1. Method holder class may get unloaded. A jweak to class holder is used to
//    checked for this, the method will not be re-compiled if this happens.
// 2. Method may get thrown away by RedefineClasses. We forbid this for all
//    recorded methods by marking them "on-stack" for RedefineClasses.
class CompilationInfo : public CHeapObj<MemTag::mtInternal> {
public:
  CompilationInfo(Method *method, int comp_level, int bci) :
      _klass_holder(JNIHandles::make_weak_global(Handle(Thread::current(), method->method_holder()->klass_holder()))),
      _method(method), _bci(bci), _comp_level(comp_level) {}
  ~CompilationInfo() {
    JNIHandles::destroy_weak_global(_klass_holder);
  }
  NONCOPYABLE(CompilationInfo);

  Method *method() const { return _method; }
  int bci() const { return _bci; };
  int comp_level() const { return _comp_level; };

  // True if the method's holder class has not yet been unloaded.
  bool is_method_alive() const {
    return _klass_holder == nullptr || !JNIHandles::is_weak_global_cleared(_klass_holder);
  }

private:
  const jweak _klass_holder;
  Method * const _method;
  const int _bci;
  const int _comp_level;
};

static void request_recompilation_if_alive(const CompilationInfo &info) {
  methodHandle mh;
  {
    const NoSafepointVerifier nsf; // Ensure the method is not unloaded while we are putting it into the handle
    if (!info.is_method_alive()) {
      log_trace(crac)("Skipping requesting recompilation: <unloaded method>, bci=%i, comp_level=%i",
                      info.bci(), info.comp_level());
      return;
    }
    mh = methodHandle(Thread::current(), info.method());
  }
  assert(Method::is_valid_method(mh()), "sanity check");

  if (log_is_enabled(Trace, crac)) {
    ResourceMark rm;
    log_trace(crac)("Requesting recompilation: %s, bci=%i, comp_level=%i",
                    info.method()->external_name(), info.bci(), info.comp_level());
  }

  auto * const THREAD = JavaThread::current();
  CompileBroker::compile_method(
    mh, info.bci(), info.comp_level(),
    methodHandle(), 0, CompileTask::Reason_CRaC, // These are only used for logging
    THREAD);
  guarantee(!HAS_PENDING_EXCEPTION, "the method should have been successfully compiled before");
}

static Mutex *decompilations_lock;
static volatile GrowableArrayCHeap<const CompilationInfo *, MemTag::mtInternal> *decompilations;

void CRaCRecompiler::start_recording_decompilations() {
  if (decompilations_lock == nullptr) {
    // Rank is nosafepoint - 1 because it should be acquirable when holding MDOExtraData_lock ranked nosafepoint
    decompilations_lock = new Mutex(Mutex::nosafepoint - 1, "CRaCRecompiler_lock");
  }
  const MutexLocker ml(decompilations_lock, Mutex::_no_safepoint_check_flag);
  assert(decompilations == nullptr, "previous recording has not been finished");
  log_debug(crac)("Starting recording decompilations");
  // release to ensuree decompilations_lock has been stored before the non-locked load in record_decompilation(),
  // fence to not proceed with C/R until the recorder threads will see the recording update
  Atomic::release_store_fence(&decompilations, new GrowableArrayCHeap<const CompilationInfo *, MemTag::mtInternal>());
}

bool CRaCRecompiler::record_decompilation(Method *method, int bci, int comp_level) {
  if (Atomic::load_acquire(&decompilations) == nullptr) {
    return false; // Fast pass to not acquire a lock when no C/R occurs (i.e. most of the time)
  }
  const MutexLocker ml(decompilations_lock, Mutex::_no_safepoint_check_flag);
  auto * const decomps = const_cast<GrowableArrayCHeap<const CompilationInfo *, MemTag::mtInternal> *>(decompilations);
  if (decomps != nullptr) { // Re-check under the lock to be safe from concurrent deletion
    // FIXME: there can be duplicate recordings, might be a good idea to use a
    //  hash table to reduce the memory footprint.
    decomps->append(new CompilationInfo(method, comp_level, bci));
    return true;
  }
  return false;
}

void CRaCRecompiler::finish_recording_decompilations_and_recompile() {
  assert(Thread::current()->is_Java_thread(), "must be called on a Java thread");
  assert(decompilations_lock != nullptr, "lock must be initialized when starting the recording");

  const GrowableArrayCHeap<const CompilationInfo *, MemTag::mtInternal> *decomps;
  {
    const MutexLocker ml(decompilations_lock, Mutex::_no_safepoint_check_flag);
    decomps = const_cast<GrowableArrayCHeap<const CompilationInfo *, MemTag::mtInternal> *>(decompilations);
    assert(decomps != nullptr, "recording has not been started");
    log_debug(crac)("Finishing recording decompilations and requesting %i recompilations", decomps->length());
    // fence should allow the recorder threads to stop locking quicker
    Atomic::release_store_fence(&decompilations, static_cast<decltype(decompilations)>(nullptr));
  }

  for (const auto *decompilation : *decomps) {
    request_recompilation_if_alive(*decompilation);
    delete decompilation;
  }
  delete decomps;
}

void CRaCRecompiler::metadata_do(void f(Metadata *)) {
  assert_at_safepoint();
  // Since we are at a safepoint no synchronization is needed
  auto * const decompilations_ = const_cast<GrowableArrayCHeap<const CompilationInfo *, MemTag::mtInternal> *>(decompilations);
  if (decompilations_ != nullptr) {
    for (const auto *decompilation : *decompilations_) {
      if (decompilation->is_method_alive()) {
        f(decompilation->method());
      }
    }
  }
}
