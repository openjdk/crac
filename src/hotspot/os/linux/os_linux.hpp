/*
 * Copyright (c) 1999, 2024, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2017, 2021, Azul Systems, Inc. All rights reserved.
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

#ifndef OS_LINUX_OS_LINUX_HPP
#define OS_LINUX_OS_LINUX_HPP

#include "runtime/os.hpp"

// os::Linux defines the interface to Linux operating systems

class os::Linux {
  friend class os;

  static int (*_pthread_getcpuclockid)(pthread_t, clockid_t *);
  static int (*_pthread_setname_np)(pthread_t, const char*);

  static address   _initial_thread_stack_bottom;
  static uintptr_t _initial_thread_stack_size;

  static const char *_libc_version;
  static const char *_libpthread_version;

  static bool _supports_fast_thread_cpu_time;

  static GrowableArray<int>* _cpu_to_node;
  static GrowableArray<int>* _nindex_to_node;

  static julong available_memory_in_container();

 protected:

  static julong _physical_memory;
  static pthread_t _main_thread;

  static julong available_memory();
  static julong free_memory();


  static void initialize_processor_count();
  static void initialize_system_info();

  static int commit_memory_impl(char* addr, size_t bytes, bool exec);
  static int commit_memory_impl(char* addr, size_t bytes,
                                size_t alignment_hint, bool exec);

  static void set_libc_version(const char *s)       { _libc_version = s; }
  static void set_libpthread_version(const char *s) { _libpthread_version = s; }

  static void rebuild_cpu_to_node_map();
  static void rebuild_nindex_to_node_map();
  static GrowableArray<int>* cpu_to_node()    { return _cpu_to_node; }
  static GrowableArray<int>* nindex_to_node()  { return _nindex_to_node; }

  static void print_process_memory_info(outputStream* st);
  static void print_system_memory_info(outputStream* st);
  static bool print_container_info(outputStream* st);
  static void print_steal_info(outputStream* st);
  static void print_distro_info(outputStream* st);
  static void print_libversion_info(outputStream* st);
  static void print_proc_sys_info(outputStream* st);
  static bool print_ld_preload_file(outputStream* st);
  static void print_uptime_info(outputStream* st);

 public:
  struct CPUPerfTicks {
    uint64_t used;
    uint64_t usedKernel;
    uint64_t total;
    uint64_t steal;
    bool     has_steal_ticks;
  };

  static int active_processor_count();
  static void kernel_version(long* major, long* minor, long* patch);

  // If kernel1 > kernel2 return  1
  // If kernel1 < kernel2 return -1
  // If kernel1 = kernel2 return  0
  static int kernel_version_compare(long major1, long minor1, long patch1,
                                    long major2, long minor2, long patch2);

  // which_logical_cpu=-1 returns accumulated ticks for all cpus.
  static bool get_tick_information(CPUPerfTicks* pticks, int which_logical_cpu);
  static bool _stack_is_executable;
  static void *dlopen_helper(const char *name, char *ebuf, int ebuflen);
  static void *dll_load_in_vmthread(const char *name, char *ebuf, int ebuflen);
  static const char *dll_path(void* lib);

  static void init_thread_fpu_state();
  static int  get_fpu_control_word();
  static void set_fpu_control_word(int fpu_control);
  static pthread_t main_thread(void)                                { return _main_thread; }
  // returns kernel thread id (similar to LWP id on Solaris), which can be
  // used to access /proc
  static pid_t gettid();

  static address   initial_thread_stack_bottom(void)                { return _initial_thread_stack_bottom; }
  static uintptr_t initial_thread_stack_size(void)                  { return _initial_thread_stack_size; }

  static julong physical_memory() { return _physical_memory; }
  static julong host_swap();

  static intptr_t* ucontext_get_sp(const ucontext_t* uc);
  static intptr_t* ucontext_get_fp(const ucontext_t* uc);

  // GNU libc and libpthread version strings
  static const char *libc_version()           { return _libc_version; }
  static const char *libpthread_version()     { return _libpthread_version; }

  static void libpthread_init();
  static void sched_getcpu_init();
  static bool libnuma_init();
  static void* libnuma_dlsym(void* handle, const char* name);
  // libnuma v2 (libnuma_1.2) symbols
  static void* libnuma_v2_dlsym(void* handle, const char* name);

  // Return default guard size for the specified thread type
  static size_t default_guard_size(os::ThreadType thr_type);

  static bool adjustStackSizeForGuardPages(); // See comments in os_linux.cpp

  static void capture_initial_stack(size_t max_size);

  // Stack overflow handling
  static bool manually_expand_stack(JavaThread * t, address addr);
  static void expand_stack_to(address bottom);

  // fast POSIX clocks support
  static void fast_thread_clock_init(void);

  static int pthread_getcpuclockid(pthread_t tid, clockid_t *clock_id) {
    return _pthread_getcpuclockid ? _pthread_getcpuclockid(tid, clock_id) : -1;
  }

  static bool supports_fast_thread_cpu_time() {
    return _supports_fast_thread_cpu_time;
  }

  static jlong fast_thread_cpu_time(clockid_t clockid);

  static jlong sendfile(int out_fd, int in_fd, jlong* offset, jlong count);

  // Determine if the vmid is the parent pid for a child in a PID namespace.
  // Return the namespace pid if so, otherwise -1.
  static int get_namespace_pid(int vmid);

  // Output structure for query_process_memory_info() (all values in KB)
  struct meminfo_t {
    ssize_t vmsize;     // current virtual size
    ssize_t vmpeak;     // peak virtual size
    ssize_t vmrss;      // current resident set size
    ssize_t vmhwm;      // peak resident set size
    ssize_t vmswap;     // swapped out
    ssize_t rssanon;    // resident set size (anonymous mappings, needs 4.5)
    ssize_t rssfile;    // resident set size (file mappings, needs 4.5)
    ssize_t rssshmem;   // resident set size (shared mappings, needs 4.5)
  };

  // Attempts to query memory information about the current process and return it in the output structure.
  // May fail (returns false) or succeed (returns true) but not all output fields are available; unavailable
  // fields will contain -1.
  static bool query_process_memory_info(meminfo_t* info);

  // Tells if the user asked for transparent huge pages.
  static bool _thp_requested;

  static void large_page_init();

  static bool thp_requested();
  static bool should_madvise_anonymous_thps();
  static bool should_madvise_shmem_thps();

  static void madvise_transparent_huge_pages(void* addr, size_t bytes);

  // Stack repair handling

  // none present

 private:
  static void numa_init();

  static void disable_numa(const char* reason, bool warning);
  typedef int (*sched_getcpu_func_t)(void);
  typedef int (*numa_node_to_cpus_func_t)(int node, unsigned long *buffer, int bufferlen);
  typedef int (*numa_node_to_cpus_v2_func_t)(int node, void *mask);
  typedef int (*numa_max_node_func_t)(void);
  typedef int (*numa_num_configured_nodes_func_t)(void);
  typedef int (*numa_available_func_t)(void);
  typedef int (*numa_tonode_memory_func_t)(void *start, size_t size, int node);
  typedef void (*numa_interleave_memory_func_t)(void *start, size_t size, unsigned long *nodemask);
  typedef void (*numa_interleave_memory_v2_func_t)(void *start, size_t size, struct bitmask* mask);
  typedef struct bitmask* (*numa_get_membind_func_t)(void);
  typedef struct bitmask* (*numa_get_interleave_mask_func_t)(void);
  typedef struct bitmask* (*numa_get_run_node_mask_func_t)(void);
  typedef long (*numa_move_pages_func_t)(int pid, unsigned long count, void **pages, const int *nodes, int *status, int flags);
  typedef void (*numa_set_preferred_func_t)(int node);
  typedef void (*numa_set_bind_policy_func_t)(int policy);
  typedef int (*numa_bitmask_isbitset_func_t)(struct bitmask *bmp, unsigned int n);
  typedef int (*numa_bitmask_equal_func_t)(struct bitmask *bmp1, struct bitmask *bmp2);
  typedef int (*numa_distance_func_t)(int node1, int node2);

  static sched_getcpu_func_t _sched_getcpu;
  static numa_node_to_cpus_func_t _numa_node_to_cpus;
  static numa_node_to_cpus_v2_func_t _numa_node_to_cpus_v2;
  static numa_max_node_func_t _numa_max_node;
  static numa_num_configured_nodes_func_t _numa_num_configured_nodes;
  static numa_available_func_t _numa_available;
  static numa_tonode_memory_func_t _numa_tonode_memory;
  static numa_interleave_memory_func_t _numa_interleave_memory;
  static numa_interleave_memory_v2_func_t _numa_interleave_memory_v2;
  static numa_set_bind_policy_func_t _numa_set_bind_policy;
  static numa_bitmask_isbitset_func_t _numa_bitmask_isbitset;
  static numa_bitmask_equal_func_t _numa_bitmask_equal;
  static numa_distance_func_t _numa_distance;
  static numa_get_membind_func_t _numa_get_membind;
  static numa_get_run_node_mask_func_t _numa_get_run_node_mask;
  static numa_get_interleave_mask_func_t _numa_get_interleave_mask;
  static numa_move_pages_func_t _numa_move_pages;
  static numa_set_preferred_func_t _numa_set_preferred;
  static unsigned long* _numa_all_nodes;
  static struct bitmask* _numa_all_nodes_ptr;
  static struct bitmask* _numa_nodes_ptr;
  static struct bitmask* _numa_interleave_bitmask;
  static struct bitmask* _numa_membind_bitmask;
  static struct bitmask* _numa_cpunodebind_bitmask;

  static void set_sched_getcpu(sched_getcpu_func_t func) { _sched_getcpu = func; }
  static void set_numa_node_to_cpus(numa_node_to_cpus_func_t func) { _numa_node_to_cpus = func; }
  static void set_numa_node_to_cpus_v2(numa_node_to_cpus_v2_func_t func) { _numa_node_to_cpus_v2 = func; }
  static void set_numa_max_node(numa_max_node_func_t func) { _numa_max_node = func; }
  static void set_numa_num_configured_nodes(numa_num_configured_nodes_func_t func) { _numa_num_configured_nodes = func; }
  static void set_numa_available(numa_available_func_t func) { _numa_available = func; }
  static void set_numa_tonode_memory(numa_tonode_memory_func_t func) { _numa_tonode_memory = func; }
  static void set_numa_interleave_memory(numa_interleave_memory_func_t func) { _numa_interleave_memory = func; }
  static void set_numa_interleave_memory_v2(numa_interleave_memory_v2_func_t func) { _numa_interleave_memory_v2 = func; }
  static void set_numa_set_bind_policy(numa_set_bind_policy_func_t func) { _numa_set_bind_policy = func; }
  static void set_numa_bitmask_isbitset(numa_bitmask_isbitset_func_t func) { _numa_bitmask_isbitset = func; }
  static void set_numa_bitmask_equal(numa_bitmask_equal_func_t func) { _numa_bitmask_equal = func; }
  static void set_numa_distance(numa_distance_func_t func) { _numa_distance = func; }
  static void set_numa_get_membind(numa_get_membind_func_t func) { _numa_get_membind = func; }
  static void set_numa_get_run_node_mask(numa_get_run_node_mask_func_t func) { _numa_get_run_node_mask = func; }
  static void set_numa_get_interleave_mask(numa_get_interleave_mask_func_t func) { _numa_get_interleave_mask = func; }
  static void set_numa_move_pages(numa_move_pages_func_t func) { _numa_move_pages = func; }
  static void set_numa_set_preferred(numa_set_preferred_func_t func) { _numa_set_preferred = func; }
  static void set_numa_all_nodes(unsigned long* ptr) { _numa_all_nodes = ptr; }
  static void set_numa_all_nodes_ptr(struct bitmask **ptr) { _numa_all_nodes_ptr = (ptr == nullptr ? nullptr : *ptr); }
  static void set_numa_nodes_ptr(struct bitmask **ptr) { _numa_nodes_ptr = (ptr == nullptr ? nullptr : *ptr); }
  static void set_numa_interleave_bitmask(struct bitmask* ptr)     { _numa_interleave_bitmask = ptr ;   }
  static void set_numa_membind_bitmask(struct bitmask* ptr)        { _numa_membind_bitmask = ptr ;      }
  static void set_numa_cpunodebind_bitmask(struct bitmask* ptr)        { _numa_cpunodebind_bitmask = ptr ;      }
  static int sched_getcpu_syscall(void);

  enum NumaAllocationPolicy{
    NotInitialized,
    Membind,
    Interleave
  };
  static NumaAllocationPolicy _current_numa_policy;

 public:
  static int sched_getcpu()  { return _sched_getcpu != nullptr ? _sched_getcpu() : -1; }
  static int numa_node_to_cpus(int node, unsigned long *buffer, int bufferlen);
  static int numa_max_node() { return _numa_max_node != nullptr ? _numa_max_node() : -1; }
  static int numa_num_configured_nodes() {
    return _numa_num_configured_nodes != nullptr ? _numa_num_configured_nodes() : -1;
  }
  static int numa_available() { return _numa_available != nullptr ? _numa_available() : -1; }
  static int numa_tonode_memory(void *start, size_t size, int node) {
    return _numa_tonode_memory != nullptr ? _numa_tonode_memory(start, size, node) : -1;
  }

  static void initialize_cpu_count() {
    initialize_processor_count();
    if (cpu_to_node() != nullptr) {
      rebuild_cpu_to_node_map();
    }
  }

  static bool is_running_in_interleave_mode() {
    return _current_numa_policy == Interleave;
  }

  static void set_configured_numa_policy(NumaAllocationPolicy numa_policy) {
    _current_numa_policy = numa_policy;
  }

  static NumaAllocationPolicy identify_numa_policy() {
    for (int node = 0; node <= Linux::numa_max_node(); node++) {
      if (Linux::_numa_bitmask_isbitset(Linux::_numa_interleave_bitmask, node)) {
        return Interleave;
      }
    }
    return Membind;
  }

  static void numa_interleave_memory(void *start, size_t size) {
    // Prefer v2 API
    if (_numa_interleave_memory_v2 != nullptr) {
      if (is_running_in_interleave_mode()) {
        _numa_interleave_memory_v2(start, size, _numa_interleave_bitmask);
      } else if (_numa_membind_bitmask != nullptr) {
        _numa_interleave_memory_v2(start, size, _numa_membind_bitmask);
      }
    } else if (_numa_interleave_memory != nullptr) {
      _numa_interleave_memory(start, size, _numa_all_nodes);
    }
  }
  static void numa_set_preferred(int node) {
    if (_numa_set_preferred != nullptr) {
      _numa_set_preferred(node);
    }
  }
  static void numa_set_bind_policy(int policy) {
    if (_numa_set_bind_policy != nullptr) {
      _numa_set_bind_policy(policy);
    }
  }
  static int numa_distance(int node1, int node2) {
    return _numa_distance != nullptr ? _numa_distance(node1, node2) : -1;
  }
  static long numa_move_pages(int pid, unsigned long count, void **pages, const int *nodes, int *status, int flags) {
    return _numa_move_pages != nullptr ? _numa_move_pages(pid, count, pages, nodes, status, flags) : -1;
  }
  static int get_node_by_cpu(int cpu_id);
  static int get_existing_num_nodes();
  // Check if numa node is configured (non-zero memory node).
  static bool is_node_in_configured_nodes(unsigned int n) {
    if (_numa_bitmask_isbitset != nullptr && _numa_all_nodes_ptr != nullptr) {
      return _numa_bitmask_isbitset(_numa_all_nodes_ptr, n);
    } else
      return false;
  }
  // Check if numa node exists in the system (including zero memory nodes).
  static bool is_node_in_existing_nodes(unsigned int n) {
    if (_numa_bitmask_isbitset != nullptr && _numa_nodes_ptr != nullptr) {
      return _numa_bitmask_isbitset(_numa_nodes_ptr, n);
    } else if (_numa_bitmask_isbitset != nullptr && _numa_all_nodes_ptr != nullptr) {
      // Not all libnuma API v2 implement numa_nodes_ptr, so it's not possible
      // to trust the API version for checking its absence. On the other hand,
      // numa_nodes_ptr found in libnuma 2.0.9 and above is the only way to get
      // a complete view of all numa nodes in the system, hence numa_nodes_ptr
      // is used to handle CPU and nodes on architectures (like PowerPC) where
      // there can exist nodes with CPUs but no memory or vice-versa and the
      // nodes may be non-contiguous. For most of the architectures, like
      // x86_64, numa_node_ptr presents the same node set as found in
      // numa_all_nodes_ptr so it's possible to use numa_all_nodes_ptr as a
      // substitute.
      return _numa_bitmask_isbitset(_numa_all_nodes_ptr, n);
    } else
      return false;
  }
  // Check if node is in bound node set.
  static bool is_node_in_bound_nodes(int node) {
    if (_numa_bitmask_isbitset != nullptr) {
      if (is_running_in_interleave_mode()) {
        return _numa_bitmask_isbitset(_numa_interleave_bitmask, node);
      } else {
        return _numa_membind_bitmask != nullptr ? _numa_bitmask_isbitset(_numa_membind_bitmask, node) : false;
      }
    }
    return false;
  }
  // Check if memory is bound to only one numa node.
  // Returns true if memory is bound to a single numa node, otherwise returns false.
  static bool is_bound_to_single_mem_node() {
    int nodes = 0;
    unsigned int node = 0;
    unsigned int highest_node_number = 0;

    struct bitmask* mem_nodes_bitmask = Linux::_numa_membind_bitmask;
    if (Linux::is_running_in_interleave_mode()) {
      mem_nodes_bitmask = Linux::_numa_interleave_bitmask;
    }

    if (mem_nodes_bitmask != nullptr && _numa_max_node != nullptr && _numa_bitmask_isbitset != nullptr) {
      highest_node_number = _numa_max_node();
    } else {
      return false;
    }

    for (node = 0; node <= highest_node_number; node++) {
      if (_numa_bitmask_isbitset(mem_nodes_bitmask, node)) {
        nodes++;
      }
    }

    if (nodes == 1) {
      return true;
    } else {
      return false;
    }
  }
  // Check if cpu and memory nodes are aligned, returns true if nodes misalign
  static bool mem_and_cpu_node_mismatch() {
    struct bitmask* mem_nodes_bitmask = Linux::_numa_membind_bitmask;
    if (Linux::is_running_in_interleave_mode()) {
      mem_nodes_bitmask = Linux::_numa_interleave_bitmask;
    }

    if (mem_nodes_bitmask == nullptr || Linux::_numa_cpunodebind_bitmask == nullptr) {
      return false;
    }

    return !_numa_bitmask_equal(mem_nodes_bitmask, Linux::_numa_cpunodebind_bitmask);
  }

  static const GrowableArray<int>* numa_nindex_to_node() {
    return _nindex_to_node;
  }

  static void* resolve_function_descriptor(void* p);

#ifdef __GLIBC__
  // os::Linux::get_mallinfo() hides the complexity of dealing with mallinfo() or
  // mallinfo2() from the user. Use this function instead of raw mallinfo/mallinfo2()
  // to keep the JVM runtime-compatible with different glibc versions.
  //
  // mallinfo2() was added with glibc (>2.32). Legacy mallinfo() was deprecated with
  // 2.33 and may vanish in future glibcs. So we may have both or either one of
  // them.
  //
  // mallinfo2() is functionally equivalent to legacy mallinfo but returns sizes as
  // 64-bit on 64-bit platforms. Legacy mallinfo uses 32-bit fields. However, legacy
  // mallinfo is still perfectly fine to use if we know the sizes cannot have wrapped.
  // For example, if the process virtual size does not exceed 4G, we cannot have
  // malloc'ed more than 4G, so the results from legacy mallinfo() can still be used.
  //
  // os::Linux::get_mallinfo() will always prefer mallinfo2() if found, but will fall back
  // to legacy mallinfo() if only that is available. In that case, it will return true
  // in *might_have_wrapped.
  struct glibc_mallinfo {
    size_t arena;
    size_t ordblks;
    size_t smblks;
    size_t hblks;
    size_t hblkhd;
    size_t usmblks;
    size_t fsmblks;
    size_t uordblks;
    size_t fordblks;
    size_t keepcost;
  };
  static void get_mallinfo(glibc_mallinfo* out, bool* might_have_wrapped);

  // Calls out to GNU extension malloc_info if available
  // otherwise does nothing and returns -2.
  static int malloc_info(FILE* stream);
#endif // GLIBC
};

#endif // OS_LINUX_OS_LINUX_HPP
