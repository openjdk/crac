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

#ifndef SHARE_GC_SHARED_COLLECTEDHEAP_HPP
#define SHARE_GC_SHARED_COLLECTEDHEAP_HPP

#include "gc/shared/gcCause.hpp"
#include "gc/shared/gcWhen.hpp"
#include "gc/shared/softRefPolicy.hpp"
#include "gc/shared/verifyOption.hpp"
#include "memory/allocation.hpp"
#include "memory/metaspace.hpp"
#include "memory/universe.hpp"
#include "oops/stackChunkOop.hpp"
#include "runtime/handles.hpp"
#include "runtime/perfDataTypes.hpp"
#include "runtime/safepoint.hpp"
#include "services/memoryUsage.hpp"
#include "utilities/debug.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/growableArray.hpp"

// A "CollectedHeap" is an implementation of a java heap for HotSpot.  This
// is an abstract class: there may be many different kinds of heaps.  This
// class defines the functions that a heap must implement, and contains
// infrastructure common to all heaps.

class GCHeapLog;
class GCHeapSummary;
class GCMemoryManager;
class GCMetaspaceLog;
class GCTimer;
class GCTracer;
class MemoryPool;
class MetaspaceSummary;
class ReservedHeapSpace;
class Thread;
class ThreadClosure;
class VirtualSpaceSummary;
class WorkerThreads;
class nmethod;

class ParallelObjectIteratorImpl : public CHeapObj<mtGC> {
public:
  virtual ~ParallelObjectIteratorImpl() {}
  virtual void object_iterate(ObjectClosure* cl, uint worker_id) = 0;
};

// User facing parallel object iterator. This is a StackObj, which ensures that
// the _impl is allocated and deleted in the scope of this object. This ensures
// the life cycle of the implementation is as required by ThreadsListHandle,
// which is sometimes used by the root iterators.
class ParallelObjectIterator : public StackObj {
  ParallelObjectIteratorImpl* _impl;

public:
  ParallelObjectIterator(uint thread_num);
  ~ParallelObjectIterator();
  void object_iterate(ObjectClosure* cl, uint worker_id);
};

//
// CollectedHeap
//   SerialHeap
//   G1CollectedHeap
//   ParallelScavengeHeap
//   ShenandoahHeap
//   ZCollectedHeap
//
class CollectedHeap : public CHeapObj<mtGC> {
  friend class VMStructs;
  friend class JVMCIVMStructs;
  friend class IsSTWGCActiveMark; // Block structured external access to _is_stw_gc_active
  friend class MemAllocator;

 private:
  GCHeapLog*      _heap_log;
  GCMetaspaceLog* _metaspace_log;

  // Historic gc information
  size_t _capacity_at_last_gc;
  size_t _used_at_last_gc;

  SoftRefPolicy _soft_ref_policy;

  // First, set it to java_lang_Object.
  // Then, set it to FillerObject after the FillerObject_klass loading is complete.
  static Klass* _filler_object_klass;

 protected:
  // Not used by all GCs
  MemRegion _reserved;

  bool _is_stw_gc_active;

  // (Minimum) Alignment reserve for TLABs and PLABs.
  static size_t _lab_alignment_reserve;
  // Used for filler objects (static, but initialized in ctor).
  static size_t _filler_array_max_size;

  bool _cleanup_unused;

  static size_t _stack_chunk_max_size; // 0 for no limit

  // Last time the whole heap has been examined in support of RMI
  // MaxObjectInspectionAge.
  // This timestamp must be monotonically non-decreasing to avoid
  // time-warp warnings.
  jlong _last_whole_heap_examined_time_ns;

  unsigned int _total_collections;          // ... started
  unsigned int _total_full_collections;     // ... started
  NOT_PRODUCT(volatile size_t _promotion_failure_alot_count;)
  NOT_PRODUCT(volatile size_t _promotion_failure_alot_gc_number;)

  // Reason for current garbage collection.  Should be set to
  // a value reflecting no collection between collections.
  GCCause::Cause _gc_cause;
  GCCause::Cause _gc_lastcause;
  PerfStringVariable* _perf_gc_cause;
  PerfStringVariable* _perf_gc_lastcause;

  // Constructor
  CollectedHeap();

  // Create a new tlab. All TLAB allocations must go through this.
  // To allow more flexible TLAB allocations min_size specifies
  // the minimum size needed, while requested_size is the requested
  // size based on ergonomics. The actually allocated size will be
  // returned in actual_size.
  virtual HeapWord* allocate_new_tlab(size_t min_size,
                                      size_t requested_size,
                                      size_t* actual_size) = 0;

  // Reinitialize tlabs before resuming mutators.
  virtual void resize_all_tlabs();

  // Raw memory allocation facilities
  // The obj and array allocate methods are covers for these methods.
  // mem_allocate() should never be
  // called to allocate TLABs, only individual objects.
  virtual HeapWord* mem_allocate(size_t size,
                                 bool* gc_overhead_limit_was_exceeded) = 0;

  // Filler object utilities.
  static inline size_t filler_array_hdr_size();

  static size_t filler_array_min_size();

protected:
  static inline void zap_filler_array_with(HeapWord* start, size_t words, juint value);
  DEBUG_ONLY(static void fill_args_check(HeapWord* start, size_t words);)
  DEBUG_ONLY(static void zap_filler_array(HeapWord* start, size_t words, bool zap = true);)

  // Fill with a single array; caller must ensure filler_array_min_size() <=
  // words <= filler_array_max_size().
  static inline void fill_with_array(HeapWord* start, size_t words, bool zap = true);

  // Fill with a single object (either an int array or a java.lang.Object).
  static inline void fill_with_object_impl(HeapWord* start, size_t words, bool zap = true);

  virtual void trace_heap(GCWhen::Type when, const GCTracer* tracer);

  // Verification functions
  DEBUG_ONLY(static void check_for_valid_allocation_state();)

 public:
  enum Name {
    None,
    Serial,
    Parallel,
    G1,
    Epsilon,
    Z,
    Shenandoah
  };

 protected:
  // Get a pointer to the derived heap object.  Used to implement
  // derived class heap() functions rather than being called directly.
  template<typename T>
  static T* named_heap(Name kind) {
    CollectedHeap* heap = Universe::heap();
    assert(heap != nullptr, "Uninitialized heap");
    assert(kind == heap->kind(), "Heap kind %u should be %u",
           static_cast<uint>(heap->kind()), static_cast<uint>(kind));
    return static_cast<T*>(heap);
  }

 public:

  static inline size_t filler_array_max_size() {
    return _filler_array_max_size;
  }

  static inline size_t stack_chunk_max_size() {
    return _stack_chunk_max_size;
  }

  static inline Klass* filler_object_klass() {
    return _filler_object_klass;
  }

  static inline void set_filler_object_klass(Klass* k) {
    _filler_object_klass = k;
  }

  virtual Name kind() const = 0;

  virtual const char* name() const = 0;

  /**
   * Returns JNI error code JNI_ENOMEM if memory could not be allocated,
   * and JNI_OK on success.
   */
  virtual jint initialize() = 0;

  // In many heaps, there will be a need to perform some initialization activities
  // after the Universe is fully formed, but before general heap allocation is allowed.
  // This is the correct place to place such initialization methods.
  virtual void post_initialize();

  // Stop any onging concurrent work and prepare for exit.
  virtual void stop() {}

  // Stop and resume concurrent GC threads interfering with safepoint operations
  virtual void safepoint_synchronize_begin() {}
  virtual void safepoint_synchronize_end() {}

  void initialize_reserved_region(const ReservedHeapSpace& rs);

  virtual size_t capacity() const = 0;
  virtual size_t used() const = 0;

  // Returns unused capacity.
  virtual size_t unused() const;

  // Historic gc information
  size_t free_at_last_gc() const { return _capacity_at_last_gc - _used_at_last_gc; }
  size_t used_at_last_gc() const { return _used_at_last_gc; }
  void update_capacity_and_used_at_gc();

  // Support for java.lang.Runtime.maxMemory():  return the maximum amount of
  // memory that the vm could make available for storing 'normal' java objects.
  // This is based on the reserved address space, but should not include space
  // that the vm uses internally for bookkeeping or temporary storage
  // (e.g., in the case of the young gen, one of the survivor
  // spaces).
  virtual size_t max_capacity() const = 0;

  // Returns "TRUE" iff "p" points into the committed areas of the heap.
  // This method can be expensive so avoid using it in performance critical
  // code.
  virtual bool is_in(const void* p) const = 0;

  DEBUG_ONLY(bool is_in_or_null(const void* p) const { return p == nullptr || is_in(p); })

  void set_gc_cause(GCCause::Cause v);
  GCCause::Cause gc_cause() { return _gc_cause; }

  oop obj_allocate(Klass* klass, size_t size, TRAPS);
  virtual oop array_allocate(Klass* klass, size_t size, int length, bool do_zero, TRAPS);
  oop class_allocate(Klass* klass, size_t size, TRAPS);

  // Utilities for turning raw memory into filler objects.
  //
  // min_fill_size() is the smallest region that can be filled.
  // fill_with_objects() can fill arbitrary-sized regions of the heap using
  // multiple objects.  fill_with_object() is for regions known to be smaller
  // than the largest array of integers; it uses a single object to fill the
  // region and has slightly less overhead.
  static size_t min_fill_size() {
    return size_t(align_object_size(oopDesc::header_size()));
  }

  static void fill_with_objects(HeapWord* start, size_t words, bool zap = true);

  static void fill_with_object(HeapWord* start, size_t words, bool zap = true);
  static void fill_with_object(MemRegion region, bool zap = true) {
    fill_with_object(region.start(), region.word_size(), zap);
  }
  static void fill_with_object(HeapWord* start, HeapWord* end, bool zap = true) {
    fill_with_object(start, pointer_delta(end, start), zap);
  }

  virtual void fill_with_dummy_object(HeapWord* start, HeapWord* end, bool zap);
  static size_t min_dummy_object_size() {
    return oopDesc::header_size();
  }

  static size_t lab_alignment_reserve() {
    assert(_lab_alignment_reserve != SIZE_MAX, "uninitialized");
    return _lab_alignment_reserve;
  }

  // Some heaps may be in an unparseable state at certain times between
  // collections. This may be necessary for efficient implementation of
  // certain allocation-related activities. Calling this function before
  // attempting to parse a heap ensures that the heap is in a parsable
  // state (provided other concurrent activity does not introduce
  // unparsability). It is normally expected, therefore, that this
  // method is invoked with the world stopped.
  // NOTE: if you override this method, make sure you call
  // super::ensure_parsability so that the non-generational
  // part of the work gets done. See implementation of
  // CollectedHeap::ensure_parsability and, for instance,
  // that of ParallelScavengeHeap::ensure_parsability().
  // The argument "retire_tlabs" controls whether existing TLABs
  // are merely filled or also retired, thus preventing further
  // allocation from them and necessitating allocation of new TLABs.
  virtual void ensure_parsability(bool retire_tlabs);

  // The amount of space available for thread-local allocation buffers.
  virtual size_t tlab_capacity(Thread *thr) const = 0;

  // The amount of used space for thread-local allocation buffers for the given thread.
  virtual size_t tlab_used(Thread *thr) const = 0;

  virtual size_t max_tlab_size() const;

  // An estimate of the maximum allocation that could be performed
  // for thread-local allocation buffers without triggering any
  // collection or expansion activity.
  virtual size_t unsafe_max_tlab_alloc(Thread *thr) const = 0;

  // Perform a collection of the heap; intended for use in implementing
  // "System.gc".  This probably implies as full a collection as the
  // "CollectedHeap" supports.
  virtual void collect(GCCause::Cause cause) = 0;

  // Perform a full collection
  virtual void do_full_collection(bool clear_all_soft_refs) = 0;

  // This interface assumes that it's being called by the
  // vm thread. It collects the heap assuming that the
  // heap lock is already held and that we are executing in
  // the context of the vm thread.
  virtual void collect_as_vm_thread(GCCause::Cause cause);

  virtual MetaWord* satisfy_failed_metadata_allocation(ClassLoaderData* loader_data,
                                                       size_t size,
                                                       Metaspace::MetadataType mdtype);

  // Return true, if accesses to the object would require barriers.
  // This is used by continuations to copy chunks of a thread stack into StackChunk object or out of a StackChunk
  // object back into the thread stack. These chunks may contain references to objects. It is crucial that
  // the GC does not attempt to traverse the object while we modify it, because its structure (oopmap) is changed
  // when stack chunks are stored into it.
  // StackChunk objects may be reused, the GC must not assume that a StackChunk object is always a freshly
  // allocated object.
  virtual bool requires_barriers(stackChunkOop obj) const = 0;

  // Returns "true" iff there is a stop-world GC in progress.
  bool is_stw_gc_active() const { return _is_stw_gc_active; }

  void set_cleanup_unused(bool value) { _cleanup_unused = value; }
  bool should_cleanup_unused() const { return _cleanup_unused; }

  // Total number of GC collections (started)
  unsigned int total_collections() const { return _total_collections; }
  unsigned int total_full_collections() const { return _total_full_collections;}

  // Increment total number of GC collections (started)
  void increment_total_collections(bool full = false) {
    _total_collections++;
    if (full) {
      _total_full_collections++;
    }
  }

  // Return the SoftRefPolicy for the heap;
  SoftRefPolicy* soft_ref_policy() { return &_soft_ref_policy; }

  virtual MemoryUsage memory_usage();
  virtual GrowableArray<GCMemoryManager*> memory_managers() = 0;
  virtual GrowableArray<MemoryPool*> memory_pools() = 0;

  // Iterate over all objects, calling "cl.do_object" on each.
  virtual void object_iterate(ObjectClosure* cl) = 0;

  virtual ParallelObjectIteratorImpl* parallel_object_iterator(uint thread_num) {
    return nullptr;
  }

  // Keep alive an object that was loaded with AS_NO_KEEPALIVE.
  virtual void keep_alive(oop obj) {}

  // Perform any cleanup actions necessary before allowing a verification.
  virtual void prepare_for_verify() = 0;

  // Returns the longest time (in ms) that has elapsed since the last
  // time that the whole heap has been examined by a garbage collection.
  jlong millis_since_last_whole_heap_examined();
  // GC should call this when the next whole heap analysis has completed to
  // satisfy above requirement.
  void record_whole_heap_examined_timestamp();

 private:
  // Generate any dumps preceding or following a full gc
  void full_gc_dump(GCTimer* timer, bool before);

  virtual void initialize_serviceability() = 0;

  void print_relative_to_gc(GCWhen::Type when) const;

 public:
  void pre_full_gc_dump(GCTimer* timer);
  void post_full_gc_dump(GCTimer* timer);

  virtual VirtualSpaceSummary create_heap_space_summary();
  GCHeapSummary create_heap_summary();

  MetaspaceSummary create_metaspace_summary();

  // GCs are free to represent the bit representation for null differently in memory,
  // which is typically not observable when using the Access API. However, if for
  // some reason a context doesn't allow using the Access API, then this function
  // explicitly checks if the given memory location contains a null value.
  virtual bool contains_null(const oop* p) const;

  void print_invocation_on(outputStream* st, const char* type, GCWhen::Type when) const;

  // Print heap information.
  virtual void print_heap_on(outputStream* st) const = 0;

  // Print additional information about the GC that is not included in print_heap_on().
  virtual void print_gc_on(outputStream* st) const = 0;

  // The default behavior is to call print_heap_on() and print_gc_on() on tty.
  virtual void print() const;

  // Used to print information about locations in the hs_err file.
  virtual bool print_location(outputStream* st, void* addr) const = 0;

  // Iterator for all GC threads (other than VM thread)
  virtual void gc_threads_do(ThreadClosure* tc) const = 0;

  // Print any relevant tracing info that flags imply.
  // Default implementation does nothing.
  virtual void print_tracing_info() const = 0;

  void print_before_gc() const;
  void print_after_gc() const;

  // Registering and unregistering an nmethod (compiled code) with the heap.
  virtual void register_nmethod(nmethod* nm) = 0;
  virtual void unregister_nmethod(nmethod* nm) = 0;
  virtual void verify_nmethod(nmethod* nm) = 0;

  void trace_heap_before_gc(const GCTracer* gc_tracer);
  void trace_heap_after_gc(const GCTracer* gc_tracer);

  // Heap verification
  virtual void verify(VerifyOption option) = 0;

  // Return true if concurrent gc control via WhiteBox is supported by
  // this collector.  The default implementation returns false.
  virtual bool supports_concurrent_gc_breakpoints() const;

  // Workers used in non-GC safepoints for parallel safepoint cleanup. If this
  // method returns null, cleanup tasks are done serially in the VMThread. See
  // `SafepointSynchronize::do_cleanup_tasks` for details.
  // GCs using a GC worker thread pool inside GC safepoints may opt to share
  // that pool with non-GC safepoints, avoiding creating extraneous threads.
  // Such sharing is safe, because GC safepoints and non-GC safepoints never
  // overlap. For example, `G1CollectedHeap::workers()` (for GC safepoints) and
  // `G1CollectedHeap::safepoint_workers()` (for non-GC safepoints) return the
  // same thread-pool.
  virtual WorkerThreads* safepoint_workers() { return nullptr; }

  // Support for object pinning. This is used by JNI Get*Critical()
  // and Release*Critical() family of functions. The GC must guarantee
  // that pinned objects never move and don't get reclaimed as garbage.
  // These functions are potentially safepointing.
  virtual void pin_object(JavaThread* thread, oop obj) = 0;
  virtual void unpin_object(JavaThread* thread, oop obj) = 0;

  // Support for loading objects from CDS archive into the heap
  // (usually as a snapshot of the old generation).
  virtual bool can_load_archived_objects() const { return false; }
  virtual HeapWord* allocate_loaded_archive_space(size_t size) { return nullptr; }
  virtual void complete_loaded_archive_space(MemRegion archive_space) { }

  virtual bool is_oop(oop object) const;
  // Non product verification and debugging.
#ifndef PRODUCT
  // Support for PromotionFailureALot.  Return true if it's time to cause a
  // promotion failure.  The no-argument version uses
  // this->_promotion_failure_alot_count as the counter.
  bool promotion_should_fail(volatile size_t* count);
  bool promotion_should_fail();

  // Reset the PromotionFailureALot counters.  Should be called at the end of a
  // GC in which promotion failure occurred.
  void reset_promotion_should_fail(volatile size_t* count);
  void reset_promotion_should_fail();
#endif  // #ifndef PRODUCT

  virtual void after_restore(void) {}
};

// Class to set and reset the GC cause for a CollectedHeap.

class GCCauseSetter : StackObj {
  CollectedHeap* _heap;
  GCCause::Cause _previous_cause;
 public:
  GCCauseSetter(CollectedHeap* heap, GCCause::Cause cause) {
    _heap = heap;
    _previous_cause = _heap->gc_cause();
    _heap->set_gc_cause(cause);
  }

  ~GCCauseSetter() {
    _heap->set_gc_cause(_previous_cause);
  }
};

#endif // SHARE_GC_SHARED_COLLECTEDHEAP_HPP
