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
#include "compiler/compilerDefinitions.hpp"
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
#include "utilities/checkedCast.hpp"
#include "utilities/debug.hpp"
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
  CompilationInfo(Method *method, int bci, int comp_level) :
      _klass_holder(JNIHandles::make_weak_global(Handle(Thread::current(), method->method_holder()->klass_holder()))),
      _method(method), _bci(bci), _comp_level(comp_level) {}
  ~CompilationInfo() {
    if (_klass_holder != nullptr) {
      if (JNIHandles::is_weak_global_handle(_klass_holder)) {
        JNIHandles::destroy_weak_global(_klass_holder);
      } else {
        JNIHandles::destroy_global(_klass_holder);
      }
    }
  }
  NONCOPYABLE(CompilationInfo);

  Method *method() const { return _method; }
  int bci() const { return _bci; };
  int comp_level() const { return _comp_level; };

  bool is_method_loaded() const {
    return _klass_holder == nullptr || // bootstrap loader is never unloaded
           JNIHandles::is_global_handle(_klass_holder) || // Strong handle keeps it loaded
           !JNIHandles::is_weak_global_cleared(_klass_holder); // Weak handle but still loaded
  }
  bool keep_method_loaded() {
    const NoSafepointVerifier nsv; // Ensure not unloaded concurrently
    if (!is_method_loaded()) {
      return false; // Already unloaded
    }
    JNIHandles::destroy_weak_global(_klass_holder);
    _klass_holder = JNIHandles::make_global(Handle(Thread::current(), method()->method_holder()->klass_holder()));
    postcond(is_method_loaded());
    return true;
  }

private:
  jweak _klass_holder;
  Method * const _method;
  const int _bci;
  const int _comp_level;
};

static void request_recompilation(CompilationInfo *info) {
  if (!info->keep_method_loaded()) {
    log_trace(crac)("Skipping recompilation: <unloaded method>, bci=%i, comp_level=%i — got unloaded",
                    info->bci(), info->comp_level());
    return;
  }
  assert(Method::is_valid_method(info->method()), "sanity check");

  if (log_is_enabled(Trace, crac)) {
    ResourceMark rm;
    log_trace(crac)("Requesting recompilation: %s, bci=%i, comp_level=%i",
                    info->method()->external_name(), info->bci(), info->comp_level());
  }

  auto * const THREAD = JavaThread::current();
  // Note: this does not guarantee the method will get compiled; e.g. there may
  // already be compilation tasks for this method (even if on another level or
  // OSR-BCI) or it may have gotten not-compilable since it was recorded.
  CompileBroker::compile_method(methodHandle(THREAD, info->method()), info->bci(), info->comp_level(),
                                methodHandle(), 0, CompileTask::Reason_CRaC, THREAD);
  guarantee(!HAS_PENDING_EXCEPTION, "the method should have been successfully compiled before");
}

static Mutex *decompilations_lock;
static volatile GrowableArrayCHeap<CompilationInfo *, MemTag::mtInternal> *decompilations;

static bool is_recording_decompilations() {
  return Atomic::load_acquire(&decompilations) != nullptr;
}

void CRaCRecompiler::start_recording_decompilations() {
  if (decompilations_lock == nullptr) {
    // Rank is nosafepoint - 1 because it should be acquirable when holding MDOExtraData_lock ranked nosafepoint
    decompilations_lock = new Mutex(Mutex::nosafepoint - 1, "CRaCRecompiler_lock");
  }
  const MutexLocker ml(decompilations_lock, Mutex::_no_safepoint_check_flag);
  precond(!is_recording_decompilations());
  log_debug(crac)("Starting recording decompilations");
  // release to ensuree decompilations_lock has been stored before the non-locked load in record_decompilation(),
  // fence to not proceed with C/R until the recorder threads will see the recording update
  Atomic::release_store_fence(&decompilations, new GrowableArrayCHeap<CompilationInfo *, MemTag::mtInternal>());
  postcond(is_recording_decompilations());
}

void CRaCRecompiler::record_decompilation(const nmethod &nmethod) {
  if (!is_recording_decompilations()) {
    return; // Fast pass to not acquire a lock when no C/R occurs (i.e. most of the time)
  }
  const MutexLocker ml(decompilations_lock, Mutex::_no_safepoint_check_flag);
  auto * const decomps = const_cast<GrowableArrayCHeap<CompilationInfo *, MemTag::mtInternal> *>(decompilations);
  if (decomps != nullptr) { // Re-check under the lock to be safe from concurrent deletion
    decomps->append(new CompilationInfo(nmethod.method(),
                                        nmethod.is_osr_method() ? nmethod.osr_entry_bci() : InvocationEntryBci,
                                        nmethod.comp_level()));
  }
}

void CRaCRecompiler::finish_recording_decompilations_and_recompile() {
  assert(Thread::current()->is_Java_thread(), "need a Java thread");
  assert(decompilations_lock != nullptr, "lock must be initialized when starting the recording");

  const GrowableArrayCHeap<CompilationInfo *, MemTag::mtInternal> *decomps;
  {
    const MutexLocker ml(decompilations_lock, Mutex::_no_safepoint_check_flag);
    precond(is_recording_decompilations());
    decomps = const_cast<GrowableArrayCHeap<CompilationInfo *, MemTag::mtInternal> *>(decompilations);
    assert(decomps != nullptr, "recording has not been started");
    log_debug(crac)("Finishing recording decompilations and requesting %i recompilations", decomps->length());
    // fence should allow the recorder threads to stop locking quicker
    Atomic::release_store_fence(&decompilations, static_cast<decltype(decompilations)>(nullptr));
    postcond(!is_recording_decompilations());
  }

  // There can only be one compilation queued/in-progress for a method at a
  // time, if there is one already for this method our request for it will just
  // be ignored.
  // TODO: we could optimize at least our own requests by placing requests for
  //  the same method further away from each other.
  for (auto * const decompilation : *decomps) {
    request_recompilation(decompilation);
    delete decompilation;
  }
  delete decomps;
}

bool CRaCRecompiler::is_recompilation_relevant(const methodHandle &method, int bci, int comp_level) {
  const nmethod *current_nmethod = bci == InvocationEntryBci ?
      method->code() :
      method->lookup_osr_nmethod_for(bci, CompLevel::CompLevel_any, false);
  const CompLevel current_comp_level = current_nmethod != nullptr ?
    checked_cast<CompLevel>(current_nmethod->comp_level()) :
    CompLevel::CompLevel_none;
  switch (current_comp_level) {
    case CompLevel::CompLevel_none:
      assert(comp_level > CompLevel::CompLevel_none, "must be compiled");
      return true; // JIT is better than interpreter
    case CompLevel::CompLevel_simple:
    case CompLevel::CompLevel_full_optimization:
      return false; // Already on a final level
    case CompLevel::CompLevel_limited_profile:
    case CompLevel::CompLevel_full_profile:
      return comp_level == CompLevel::CompLevel_full_optimization; // C2 is better than C1
    default:
      ShouldNotReachHere();
      return false;
  }
}

void CRaCRecompiler::metadata_do(void f(Metadata *)) {
  assert_at_safepoint();
  // Since we are at a safepoint no synchronization is needed
  auto * const decomps = const_cast<GrowableArrayCHeap<CompilationInfo *, MemTag::mtInternal> *>(decompilations);
  if (decomps != nullptr) {
    for (const auto *decompilation : *decomps) {
      if (decompilation->is_method_loaded()) {
        f(decompilation->method());
      }
    }
  }
}
