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
#include "runtime/handles.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"

#ifdef  _WINDOWS
# include <io.h>
# define close _close
#else
# include <unistd.h>
#endif // _WINDOWS

// xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
#define UUID_LENGTH 36
#define BAD_OFFSET 0xFFFFFFFFBAD0FF5Eull

class crac: AllStatic {
public:
  static void vm_create_start();
  static bool prepare_checkpoint();
  static Handle checkpoint(jarray fd_arr, jobjectArray obj_arr, bool dry_run, jlong jcmd_stream, TRAPS);
  static void restore();

  static jlong restore_start_time();
  static jlong uptime_since_restore();

  static void record_time_before_checkpoint();
  static void update_javaTimeNanos_offset();

  static jlong monotonic_time_offset() {
    return javaTimeNanos_offset;
  }

  class MemoryWriter: public CHeapObj<mtInternal> {
  protected:
    int _fd;
    size_t _offset_curr;

  public:
    MemoryWriter(const char *filename);
    virtual ~MemoryWriter() {
      if (_fd >= 0) {
        ::close(_fd);
      }
    }
    virtual size_t write(void *addr, size_t size) = 0;
  };

  class MemoryReader: public CHeapObj<mtInternal> {
  protected:
    int _fd;

  public:
    MemoryReader(const char *filename);
    virtual ~MemoryReader() {
      if (_fd >= 0) {
        ::close(_fd);
      }
    }
    virtual void read(size_t offset, void *addr, size_t size, bool executable) = 0;
  };

  class MmappingMemoryReader: public crac::MemoryReader {
  public:
    MmappingMemoryReader(const char *filename): MemoryReader(filename) {}
    void read(size_t offset, void *addr, size_t size, bool executable) override;
  };

  class FileMemoryReader: public crac::MemoryReader {
  public:
    FileMemoryReader(const char *filename): MemoryReader(filename) {}
    void read(size_t offset, void *addr, size_t size, bool executable) override;
  };

  class MemoryPersister: AllStatic {
  protected:
    enum Flags {
      DATA       = 1 << 0,
      EXECUTABLE = 1 << 1,
      ACCESSIBLE = 1 << 2,
    };

    struct record {
      u_int64_t addr;
      u_int64_t length;
      u_int64_t offset;
      int flags;
    };

    class SearchInIndex: public CompareClosure<struct record> {
    public:
      int do_compare(const struct record &a, const struct record &b) {
        if (a.addr < b.addr) return -1;
        if (a.addr > b.addr) return 1;
        return 0;
      }
    };

    static void allocate_index(size_t slots);

    static GrowableArray<struct crac::MemoryPersister::record> _index;
    static MemoryWriter *_writer;

  public:
    static constexpr char MEMORY_IMG[] = "memory.img";

    static void init();
    static bool store(void *addr, size_t length, size_t mapped_length, bool executable);
    static bool store_gap(void *addr, size_t length);

    static void finalize();
    static void load_on_restore();
#ifdef ASSERT
    static void assert_mem(void *addr, size_t used, size_t total);
    static void assert_gap(void *addr, size_t length);
#endif // ASSERT
  private:
    static bool unmap(void *addr, size_t length);
    static bool map(void *addr, size_t length, bool executable);
    static bool map_gap(void *addr, size_t length);
  };

  static void before_threads_persisted();
  static void after_threads_restored();

private:
  static bool read_all(int fd, char *buf, size_t bytes);
  static bool read_bootid(char *dest);

  static jlong checkpoint_millis;
  static jlong checkpoint_nanos;
  static char checkpoint_bootid[UUID_LENGTH];
  static jlong javaTimeNanos_offset;
};

class CountThreadsClosure: public ThreadClosure {
private:
  size_t _count;
public:
  CountThreadsClosure(): _count(0) {}

  void do_thread(Thread* t) {
    ++_count;
  }

  size_t count() {
    return _count;
  }
};

#endif //SHARE_RUNTIME_CRAC_HPP
