/*
 * Copyright (c) 2001, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "cds/cdsConfig.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/vmClasses.hpp"
#include "gc/shared/allocTracer.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/gcHeapSummary.hpp"
#include "gc/shared/gcLocker.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "gc/shared/gcWhen.hpp"
#include "gc/shared/memAllocator.hpp"
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/classLoaderMetaspace.hpp"
#include "memory/metaspace.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/reservedSpace.hpp"
#include "memory/universe.hpp"
#include "oops/instanceMirrorKlass.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/init.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/perfData.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vmThread.hpp"
#include "services/heapDumper.hpp"
#include "utilities/align.hpp"
#include "utilities/copy.hpp"
#include "utilities/events.hpp"
#include "utilities/ostream.hpp"

class ClassLoaderData;

size_t CollectedHeap::_lab_alignment_reserve = SIZE_MAX;
Klass* CollectedHeap::_filler_object_klass = nullptr;
size_t CollectedHeap::_filler_array_max_size = 0;
size_t CollectedHeap::_stack_chunk_max_size = 0;

class GCLogMessage : public FormatBuffer<512> {};

template <>
void EventLogBase<GCLogMessage>::print(outputStream* st, GCLogMessage& m) {
  st->print_raw(m);
}

class GCLog : public EventLogBase<GCLogMessage> {
 protected:
  virtual void log_usage(const CollectedHeap* heap, outputStream* st) const = 0;

 public:
  GCLog(const char* name, const char* handle) : EventLogBase<GCLogMessage>(name, handle) {}

  void log_gc(const CollectedHeap* heap, GCWhen::Type when);
};

void GCLog::log_gc(const CollectedHeap* heap, GCWhen::Type when) {
  if (!should_log()) {
    return;
  }

  double timestamp = fetch_timestamp();
  MutexLocker ml(&_mutex, Mutex::_no_safepoint_check_flag);
  int index = compute_log_index();
  _records[index].thread = nullptr; // It's the GC thread so it's not that interesting.
  _records[index].timestamp = timestamp;
  stringStream st(_records[index].data.buffer(), _records[index].data.size());

  st.print("{");
  {
    heap->print_invocation_on(&st, _handle, when);
    StreamIndentor si(&st, 1);
    log_usage(heap, &st);
  }
  st.print_cr("}");
}

class GCHeapLog : public GCLog {
 private:
  void log_usage(const CollectedHeap* heap, outputStream* st) const override {
    heap->print_heap_on(st);
  }

 public:
  GCHeapLog() : GCLog("GC Heap Usage History", "heap") {}
};

class GCMetaspaceLog : public GCLog {
 private:
  void log_usage(const CollectedHeap* heap, outputStream* st) const override {
    MetaspaceUtils::print_on(st);
  }

 public:
  GCMetaspaceLog() : GCLog("Metaspace Usage History", "metaspace") {}
};

ParallelObjectIterator::ParallelObjectIterator(uint thread_num) :
  _impl(Universe::heap()->parallel_object_iterator(thread_num))
{}

ParallelObjectIterator::~ParallelObjectIterator() {
  delete _impl;
}

void ParallelObjectIterator::object_iterate(ObjectClosure* cl, uint worker_id) {
  _impl->object_iterate(cl, worker_id);
}

size_t CollectedHeap::unused() const {
  MutexLocker ml(Heap_lock);
  return capacity() - used();
}

VirtualSpaceSummary CollectedHeap::create_heap_space_summary() {
  size_t capacity_in_words = capacity() / HeapWordSize;

  return VirtualSpaceSummary(
    _reserved.start(), _reserved.start() + capacity_in_words, _reserved.end());
}

GCHeapSummary CollectedHeap::create_heap_summary() {
  VirtualSpaceSummary heap_space = create_heap_space_summary();
  return GCHeapSummary(heap_space, used());
}

MetaspaceSummary CollectedHeap::create_metaspace_summary() {
  const MetaspaceChunkFreeListSummary& ms_chunk_free_list_summary =
    MetaspaceUtils::chunk_free_list_summary(Metaspace::NonClassType);
  const MetaspaceChunkFreeListSummary& class_chunk_free_list_summary =
    MetaspaceUtils::chunk_free_list_summary(Metaspace::ClassType);
  return MetaspaceSummary(MetaspaceGC::capacity_until_GC(),
                          MetaspaceUtils::get_combined_statistics(),
                          ms_chunk_free_list_summary, class_chunk_free_list_summary);
}

bool CollectedHeap::contains_null(const oop* p) const {
  return *p == nullptr;
}

void CollectedHeap::print_invocation_on(outputStream* st, const char* type, GCWhen::Type when) const {
  st->print_cr("%s %s invocations=%u (full %u):", type, GCWhen::to_string(when), total_collections(), total_full_collections());
}

void CollectedHeap::print_relative_to_gc(GCWhen::Type when) const {
  // Print heap information
  LogTarget(Debug, gc, heap) lt_heap;
  if (lt_heap.is_enabled()) {
    LogStream ls(lt_heap);
    print_invocation_on(&ls, "Heap", when);
    StreamIndentor si(&ls, 1);
    print_heap_on(&ls);
  }

  if (_heap_log != nullptr) {
    _heap_log->log_gc(this, when);
  }

  // Print metaspace information
  LogTarget(Debug, gc, metaspace) lt_metaspace;
  if (lt_metaspace.is_enabled()) {
    LogStream ls(lt_metaspace);
    print_invocation_on(&ls, "Metaspace", when);
    StreamIndentor indentor(&ls, 1);
    MetaspaceUtils::print_on(&ls);
  }

  if (_metaspace_log != nullptr) {
    _metaspace_log->log_gc(this, when);
  }
}

void CollectedHeap::print_before_gc() const {
  print_relative_to_gc(GCWhen::BeforeGC);
}

void CollectedHeap::print_after_gc() const {
  print_relative_to_gc(GCWhen::AfterGC);
}

void CollectedHeap::print() const {
  print_heap_on(tty);
  print_gc_on(tty);
}

void CollectedHeap::trace_heap(GCWhen::Type when, const GCTracer* gc_tracer) {
  const GCHeapSummary& heap_summary = create_heap_summary();
  gc_tracer->report_gc_heap_summary(when, heap_summary);

  const MetaspaceSummary& metaspace_summary = create_metaspace_summary();
  gc_tracer->report_metaspace_summary(when, metaspace_summary);
}

void CollectedHeap::trace_heap_before_gc(const GCTracer* gc_tracer) {
  trace_heap(GCWhen::BeforeGC, gc_tracer);
}

void CollectedHeap::trace_heap_after_gc(const GCTracer* gc_tracer) {
  trace_heap(GCWhen::AfterGC, gc_tracer);
}

// Default implementation, for collectors that don't support the feature.
bool CollectedHeap::supports_concurrent_gc_breakpoints() const {
  return false;
}

static bool klass_is_sane(oop object) {
  if (UseCompactObjectHeaders) {
    // With compact headers, we can't safely access the Klass* when
    // the object has been forwarded, because non-full-GC-forwarding
    // temporarily overwrites the mark-word, and thus the Klass*, with
    // the forwarding pointer, and here we have no way to make a
    // distinction between Full-GC and regular GC forwarding.
    markWord mark = object->mark();
    if (mark.is_forwarded()) {
      // We can't access the Klass*. We optimistically assume that
      // it is ok. This happens very rarely.
      return true;
    }

    return Metaspace::contains(mark.klass_without_asserts());
  }

  return Metaspace::contains(object->klass_without_asserts());
}

bool CollectedHeap::is_oop(oop object) const {
  if (!is_object_aligned(object)) {
    return false;
  }

  if (!is_in(object)) {
    return false;
  }

  if (!klass_is_sane(object)) {
    return false;
  }

  return true;
}

// Memory state functions.


CollectedHeap::CollectedHeap() :
  _capacity_at_last_gc(0),
  _used_at_last_gc(0),
  _soft_ref_policy(),
  _is_stw_gc_active(false),
  _cleanup_unused(false),
  _last_whole_heap_examined_time_ns(os::javaTimeNanos()),
  _total_collections(0),
  _total_full_collections(0),
  _gc_cause(GCCause::_no_gc),
  _gc_lastcause(GCCause::_no_gc)
{
  // If the minimum object size is greater than MinObjAlignment, we can
  // end up with a shard at the end of the buffer that's smaller than
  // the smallest object.  We can't allow that because the buffer must
  // look like it's full of objects when we retire it, so we make
  // sure we have enough space for a filler int array object.
  size_t min_size = min_dummy_object_size();
  _lab_alignment_reserve = min_size > (size_t)MinObjAlignment ? align_object_size(min_size) : 0;

  const size_t max_len = size_t(arrayOopDesc::max_array_length(T_INT));
  const size_t elements_per_word = HeapWordSize / sizeof(jint);
  _filler_array_max_size = align_object_size(filler_array_hdr_size() +
                                             max_len / elements_per_word);

  NOT_PRODUCT(_promotion_failure_alot_count = 0;)
  NOT_PRODUCT(_promotion_failure_alot_gc_number = 0;)

  if (UsePerfData) {
    EXCEPTION_MARK;

    // create the gc cause jvmstat counters
    _perf_gc_cause = PerfDataManager::create_string_variable(SUN_GC, "cause",
                             80, GCCause::to_string(_gc_cause), CHECK);

    _perf_gc_lastcause =
                PerfDataManager::create_string_variable(SUN_GC, "lastCause",
                             80, GCCause::to_string(_gc_lastcause), CHECK);
  }

  // Create the ring log
  if (LogEvents) {
    _metaspace_log = new GCMetaspaceLog();
    _heap_log = new GCHeapLog();
  } else {
    _metaspace_log = nullptr;
    _heap_log = nullptr;
  }
}

// This interface assumes that it's being called by the
// vm thread. It collects the heap assuming that the
// heap lock is already held and that we are executing in
// the context of the vm thread.
void CollectedHeap::collect_as_vm_thread(GCCause::Cause cause) {
  Thread* thread = Thread::current();
  assert(thread->is_VM_thread(), "Precondition#1");
  assert(Heap_lock->is_locked(), "Precondition#2");
  GCCauseSetter gcs(this, cause);
  switch (cause) {
    case GCCause::_codecache_GC_threshold:
    case GCCause::_codecache_GC_aggressive:
    case GCCause::_heap_inspection:
    case GCCause::_heap_dump:
    case GCCause::_metadata_GC_threshold: {
      HandleMark hm(thread);
      do_full_collection(false);        // don't clear all soft refs
      break;
    }
    case GCCause::_metadata_GC_clear_soft_refs: {
      HandleMark hm(thread);
      do_full_collection(true);         // do clear all soft refs
      break;
    }
    default:
      ShouldNotReachHere(); // Unexpected use of this function
  }
}

MetaWord* CollectedHeap::satisfy_failed_metadata_allocation(ClassLoaderData* loader_data,
                                                            size_t word_size,
                                                            Metaspace::MetadataType mdtype) {
  uint loop_count = 0;
  uint gc_count = 0;
  uint full_gc_count = 0;

  assert(!Heap_lock->owned_by_self(), "Should not be holding the Heap_lock");

  do {
    MetaWord* result = loader_data->metaspace_non_null()->allocate(word_size, mdtype);
    if (result != nullptr) {
      return result;
    }

    {  // Need lock to get self consistent gc_count's
      MutexLocker ml(Heap_lock);
      gc_count      = total_collections();
      full_gc_count = total_full_collections();
    }

    // Generate a VM operation
    VM_CollectForMetadataAllocation op(loader_data,
                                       word_size,
                                       mdtype,
                                       gc_count,
                                       full_gc_count,
                                       GCCause::_metadata_GC_threshold);

    VMThread::execute(&op);

    if (op.gc_succeeded()) {
      return op.result();
    }
    loop_count++;
    if ((QueuedAllocationWarningCount > 0) &&
        (loop_count % QueuedAllocationWarningCount == 0)) {
      log_warning(gc, ergo)("satisfy_failed_metadata_allocation() retries %d times,"
                            " size=%zu", loop_count, word_size);
    }
  } while (true);  // Until a GC is done
}

MemoryUsage CollectedHeap::memory_usage() {
  return MemoryUsage(InitialHeapSize, used(), capacity(), max_capacity());
}

void CollectedHeap::set_gc_cause(GCCause::Cause v) {
  if (UsePerfData) {
    _gc_lastcause = _gc_cause;
    _perf_gc_lastcause->set_value(GCCause::to_string(_gc_lastcause));
    _perf_gc_cause->set_value(GCCause::to_string(v));
  }
  _gc_cause = v;
}

// Returns the header size in words aligned to the requirements of the
// array object type.
static int int_array_header_size() {
  size_t typesize_in_bytes = arrayOopDesc::header_size_in_bytes();
  return (int)align_up(typesize_in_bytes, HeapWordSize)/HeapWordSize;
}

size_t CollectedHeap::max_tlab_size() const {
  // TLABs can't be bigger than we can fill with a int[Integer.MAX_VALUE].
  // This restriction could be removed by enabling filling with multiple arrays.
  // If we compute that the reasonable way as
  //    header_size + ((sizeof(jint) * max_jint) / HeapWordSize)
  // we'll overflow on the multiply, so we do the divide first.
  // We actually lose a little by dividing first,
  // but that just makes the TLAB  somewhat smaller than the biggest array,
  // which is fine, since we'll be able to fill that.
  size_t max_int_size = int_array_header_size() +
              sizeof(jint) *
              ((juint) max_jint / (size_t) HeapWordSize);
  return align_down(max_int_size, MinObjAlignment);
}

size_t CollectedHeap::filler_array_hdr_size() {
  return align_object_offset(int_array_header_size()); // align to Long
}

size_t CollectedHeap::filler_array_min_size() {
  return align_object_size(filler_array_hdr_size()); // align to MinObjAlignment
}

void CollectedHeap::zap_filler_array_with(HeapWord* start, size_t words, juint value) {
  Copy::fill_to_words(start + filler_array_hdr_size(),
                      words - filler_array_hdr_size(), value);
}

#ifdef ASSERT
void CollectedHeap::fill_args_check(HeapWord* start, size_t words)
{
  assert(words >= min_fill_size(), "too small to fill");
  assert(is_object_aligned(words), "unaligned size");
}

void CollectedHeap::zap_filler_array(HeapWord* start, size_t words, bool zap)
{
  if (ZapFillerObjects && zap) {
    zap_filler_array_with(start, words, 0XDEAFBABE);
  }
}
#endif // ASSERT

void
CollectedHeap::fill_with_array(HeapWord* start, size_t words, bool zap)
{
  assert(words >= filler_array_min_size(), "too small for an array");
  assert(words <= filler_array_max_size(), "too big for a single object");

  const size_t payload_size = words - filler_array_hdr_size();
  const size_t len = payload_size * HeapWordSize / sizeof(jint);
  assert((int)len >= 0, "size too large %zu becomes %d", words, (int)len);

  ObjArrayAllocator allocator(Universe::fillerArrayKlass(), words, (int)len, /* do_zero */ false);
  allocator.initialize(start);
  if (CDSConfig::is_dumping_heap()) {
    // This array is written into the CDS archive. Make sure it
    // has deterministic contents.
    zap_filler_array_with(start, words, 0);
  } else {
    DEBUG_ONLY(zap_filler_array(start, words, zap);)
  }
}

void
CollectedHeap::fill_with_object_impl(HeapWord* start, size_t words, bool zap)
{
  assert(words <= filler_array_max_size(), "too big for a single object");

  if (words >= filler_array_min_size()) {
    fill_with_array(start, words, zap);
  } else if (words > 0) {
    assert(words == min_fill_size(), "unaligned size");
    ObjAllocator allocator(CollectedHeap::filler_object_klass(), words);
    allocator.initialize(start);
  }
}

void CollectedHeap::fill_with_object(HeapWord* start, size_t words, bool zap)
{
  DEBUG_ONLY(fill_args_check(start, words);)
  HandleMark hm(Thread::current());  // Free handles before leaving.
  fill_with_object_impl(start, words, zap);
}

void CollectedHeap::fill_with_objects(HeapWord* start, size_t words, bool zap)
{
  DEBUG_ONLY(fill_args_check(start, words);)
  HandleMark hm(Thread::current());  // Free handles before leaving.

  // Multiple objects may be required depending on the filler array maximum size. Fill
  // the range up to that with objects that are filler_array_max_size sized. The
  // remainder is filled with a single object.
  const size_t min = min_fill_size();
  const size_t max = filler_array_max_size();
  while (words > max) {
    const size_t cur = (words - max) >= min ? max : max - min;
    fill_with_array(start, cur, zap);
    start += cur;
    words -= cur;
  }

  fill_with_object_impl(start, words, zap);
}

void CollectedHeap::fill_with_dummy_object(HeapWord* start, HeapWord* end, bool zap) {
  CollectedHeap::fill_with_object(start, end, zap);
}

void CollectedHeap::ensure_parsability(bool retire_tlabs) {
  assert(SafepointSynchronize::is_at_safepoint() || !is_init_completed(),
         "Should only be called at a safepoint or at start-up");

  ThreadLocalAllocStats stats;

  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *thread = jtiwh.next();) {
    BarrierSet::barrier_set()->make_parsable(thread);
    if (UseTLAB) {
      if (retire_tlabs || ZeroTLAB) {
        thread->retire_tlab(&stats);
      } else {
        thread->tlab().make_parsable();
      }
    }
  }

  stats.publish();
}

void CollectedHeap::resize_all_tlabs() {
  assert(SafepointSynchronize::is_at_safepoint() || !is_init_completed(),
         "Should only resize tlabs at safepoint");

  if (UseTLAB && ResizeTLAB) {
    for (JavaThreadIteratorWithHandle jtiwh; JavaThread *thread = jtiwh.next(); ) {
      thread->tlab().resize();
    }
  }
}

jlong CollectedHeap::millis_since_last_whole_heap_examined() {
  return (os::javaTimeNanos() - _last_whole_heap_examined_time_ns) / NANOSECS_PER_MILLISEC;
}

void CollectedHeap::record_whole_heap_examined_timestamp() {
  _last_whole_heap_examined_time_ns = os::javaTimeNanos();
}

void CollectedHeap::full_gc_dump(GCTimer* timer, bool before) {
  assert(timer != nullptr, "timer is null");
  static uint count = 0;
  if ((HeapDumpBeforeFullGC && before) || (HeapDumpAfterFullGC && !before)) {
    if (FullGCHeapDumpLimit == 0 || count < FullGCHeapDumpLimit) {
      GCTraceTime(Info, gc) tm(before ? "Heap Dump (before full gc)" : "Heap Dump (after full gc)", timer);
      HeapDumper::dump_heap();
      count++;
    }
  }

  LogTarget(Trace, gc, classhisto) lt;
  if (lt.is_enabled()) {
    GCTraceTime(Trace, gc, classhisto) tm(before ? "Class Histogram (before full gc)" : "Class Histogram (after full gc)", timer);
    LogStream ls(lt);
    VM_GC_HeapInspection inspector(&ls, false /* ! full gc */);
    inspector.doit();
  }
}

void CollectedHeap::pre_full_gc_dump(GCTimer* timer) {
  full_gc_dump(timer, true);
}

void CollectedHeap::post_full_gc_dump(GCTimer* timer) {
  full_gc_dump(timer, false);
}

void CollectedHeap::initialize_reserved_region(const ReservedHeapSpace& rs) {
  // It is important to do this in a way such that concurrent readers can't
  // temporarily think something is in the heap.  (Seen this happen in asserts.)
  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)rs.base());
  _reserved.set_end((HeapWord*)rs.end());
}

void CollectedHeap::post_initialize() {
  StringDedup::initialize();
  initialize_serviceability();
}

#ifndef PRODUCT

bool CollectedHeap::promotion_should_fail(volatile size_t* count) {
  // Access to count is not atomic; the value does not have to be exact.
  if (PromotionFailureALot) {
    const size_t gc_num = total_collections();
    const size_t elapsed_gcs = gc_num - _promotion_failure_alot_gc_number;
    if (elapsed_gcs >= PromotionFailureALotInterval) {
      // Test for unsigned arithmetic wrap-around.
      if (++*count >= PromotionFailureALotCount) {
        *count = 0;
        return true;
      }
    }
  }
  return false;
}

bool CollectedHeap::promotion_should_fail() {
  return promotion_should_fail(&_promotion_failure_alot_count);
}

void CollectedHeap::reset_promotion_should_fail(volatile size_t* count) {
  if (PromotionFailureALot) {
    _promotion_failure_alot_gc_number = total_collections();
    *count = 0;
  }
}

void CollectedHeap::reset_promotion_should_fail() {
  reset_promotion_should_fail(&_promotion_failure_alot_count);
}

#endif  // #ifndef PRODUCT

// It's the caller's responsibility to ensure glitch-freedom
// (if required).
void CollectedHeap::update_capacity_and_used_at_gc() {
  _capacity_at_last_gc = capacity();
  _used_at_last_gc     = used();
}
