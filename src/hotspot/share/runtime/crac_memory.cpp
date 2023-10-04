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
#include <limits.h>
#include <string.h>

#include "memory/resourceArea.hpp"
#include "runtime/crac.hpp"
#include "runtime/os.hpp"

const char crac::MemoryPersister::MEMORY_IMG[11];
GrowableArray<struct crac::MemoryPersister::record> crac::MemoryPersister::_index(256, mtInternal);
crac::MemoryWriter *crac::MemoryPersister::_writer = nullptr;

class FileMemoryWriter: public crac::MemoryWriter {
private:
  size_t _alignment;
public:
  FileMemoryWriter(const char *filename, size_t alignment): MemoryWriter(filename), _alignment(alignment) {}

  size_t write(void *addr, size_t size) override {
    if (!os::write(_fd, addr, size)) {
      tty->print_cr("Cannot store persisted memory: %s", os::strerror(errno));
      return BAD_OFFSET;
    }
    size_t prev_offset = _offset_curr;
    _offset_curr += size;
    if (_alignment) {
      size_t off = align_up(_offset_curr, _alignment);
      if (off > _offset_curr) {
        if (os::seek_to_file_offset(_fd, off) < 0) {
          tty->print_cr("Cannot seek: %s", os::strerror(errno));
          return false;
        }
        _offset_curr = off;
      }
    }
    return prev_offset;
  }
};

crac::MemoryWriter::MemoryWriter(const char *filename): _offset_curr(0) {
  char path[PATH_MAX];
  snprintf(path, PATH_MAX, "%s%s%s", CRaCCheckpointTo, os::file_separator(), filename);
  _fd = os::open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (_fd < 0) {
    fatal("Cannot open persisted memory file %s: %s", path, os::strerror(errno));
  }
  _offset_curr = 0;
}

crac::MemoryReader::MemoryReader(const char *filename) {
  char path[PATH_MAX];
  // When the checkpoint fails, we need to load the memory from CRaCCheckpointTo
  const char *dir = CRaCRestoreFrom != nullptr ? CRaCRestoreFrom : CRaCCheckpointTo;
  snprintf(path, PATH_MAX, "%s%s%s", dir, os::file_separator(), filename);
  _fd = os::open(path, O_RDONLY, S_IRUSR | S_IWUSR);
  if (_fd < 0) {
    fatal("Cannot open persisted memory file %s: %s", path, os::strerror(errno));
  }
}

void crac::FileMemoryReader::read(size_t offset, void *addr, size_t length, bool executable) {
  assert(_fd >= 0, "File descriptor not open");
  if (os::seek_to_file_offset(_fd, offset) < 0) {
    fatal("Cannot seek in persisted memory file: %d, 0x%zx: %s", _fd, offset, os::strerror(errno));
  }
  if (!read_all(_fd, (char *) addr, length)) {
    fatal("Cannot read persisted memory file at %p (0x%zx = %zu): %s", addr, length, length, os::strerror(errno));
  }
};

void crac::MemoryPersister::init() {
  _writer = new FileMemoryWriter(MEMORY_IMG, os::vm_page_size());
  _index.clear();
}

static bool is_all_zeroes(void *addr, size_t page_size) {
  unsigned long long *ptr = (unsigned long long *) addr;
  unsigned long long *end = (unsigned long long *)((address) addr + page_size);
  while (ptr < end && *ptr == 0) ++ptr;
  return ptr == end;
}

bool crac::MemoryPersister::store(void *addr, size_t length, size_t mapped_length, bool executable) {
  if (mapped_length == 0) {
    return true;
  }

  size_t page_size = os::vm_page_size();
  assert(is_aligned(addr, page_size), "Unaligned address %p", addr);
  assert(length <= mapped_length, "Useful length %zx longer than mapped %zx", length, mapped_length);
  assert(is_aligned(mapped_length, page_size), "Unaligned length %zx at %p (page size %zx)", mapped_length, addr, page_size);

  int execFlag = (executable ? Flags::EXECUTABLE : 0);
  address curr = (address) addr;
  address end = curr + length;
  bool do_zeroes = is_all_zeroes(addr, page_size);
  while (curr < end) {
    address start = curr;
    if (do_zeroes) {
      do {
        curr += page_size;
      } while (curr < end && is_all_zeroes(curr, page_size));
      _index.append({
        .addr = start,
        .length = (size_t) (curr - start),
        .offset = BAD_OFFSET,
        .flags = Flags::ACCESSIBLE | execFlag
      });
      do_zeroes = false;
    } else {
      do {
        curr += page_size;
      } while (curr < end && !is_all_zeroes(curr, page_size));
      size_t to_write = (curr > end ? end : curr) - start;
      size_t offset = _writer->write(start, to_write);
      _index.append({
        .addr = start,
        .length = to_write,
        .offset = offset,
        .flags = Flags::DATA | Flags::ACCESSIBLE | execFlag
      });
      do_zeroes = true;
    }
  }

  size_t aligned_length = align_up(length, page_size);
  if (aligned_length < mapped_length) {
    _index.append({
      .addr = (address) addr + aligned_length,
      .length = mapped_length - aligned_length,
      .offset = BAD_OFFSET,
      .flags = Flags::ACCESSIBLE | execFlag
    });
  }
  return unmap(addr, mapped_length);
}

bool crac::MemoryPersister::store_gap(void *addr, size_t length) {
  assert(is_aligned(addr, os::vm_page_size()), "Unaligned address");
  assert(is_aligned(length, os::vm_page_size()), "Unaligned length");
  if (length == 0) {
    return true;
  }
#ifdef ASSERT
  for (int i = 0; i < _index.length(); ++i) {
    const struct record &r = _index.at(i);
    assert((address) addr + length <= r.addr || r.addr + r.length <= addr,
      "Overlapping regions %p-%p and %p-%p", r.addr, r.addr + r.length, addr, (address) addr + length);
  }
#endif
  _index.append({
    .addr = (address) addr,
    .length = length,
    .offset = BAD_OFFSET,
    .flags = 0
  });
  return unmap(addr, length);
}

void crac::MemoryPersister::reinit_memory() {
  size_t page_size = os::vm_page_size();
  for (int i = 0; i < _index.length(); ++i) {
    const record &r = _index.at(i);
    size_t aligned_length = align_up(r.length, page_size);
    if (!map_gap(r.addr, aligned_length)) {
      fatal("Cannot reinit non-accessible memory at %p-%p", r.addr, r.addr + aligned_length);
    }
  }
}

void crac::MemoryPersister::load_on_restore() {
  MemoryReader *reader;
  bool update_protection = false;
  // When pauseengine/simengine is used we can do repeated checkpoints;
  // when the memory is mmapped and we try to write it second time, the file
  // would be truncated and subsequent attempt to read the data could cause SIGBUS.
  if (CREngine != nullptr && (!strncmp(CREngine, "pauseengine", 10) || !strncmp(CREngine, "simengine", 8))) {
    reader = new FileMemoryReader(MEMORY_IMG);
    update_protection = true;
  } else {
    reader = new MmappingMemoryReader(MEMORY_IMG);
  }
  size_t page_size = os::vm_page_size();
  for (int i = 0; i < _index.length(); ++i) {
    const record &r = _index.at(i);
    size_t aligned_length = align_up(r.length, page_size);
    bool executable = r.flags & Flags::EXECUTABLE;
    if (r.flags & Flags::ACCESSIBLE) {
      if ((r.flags & Flags::DATA) == 0) {
        if (!map(r.addr, aligned_length, executable)) {
          fatal("Cannot remap memory at %p-%p", r.addr, r.addr + aligned_length);
        }
      } else {
        char *data = (char *) r.addr;
        if (update_protection && !os::protect_memory(data, aligned_length,
            executable ? os::ProtType::MEM_PROT_RWX : os::ProtType::MEM_PROT_RW)) {
          fatal("Cannot remap memory at %p-%p", r.addr, r.addr + aligned_length);
        }
        reader->read(r.offset, data, r.length, r.flags & Flags::EXECUTABLE);
      }
    }
  }
  delete reader;
}


#ifdef ASSERT
void crac::MemoryPersister::assert_mem(void *addr, size_t used, size_t total) {
  assert(is_aligned(addr, os::vm_page_size()), "Unaligned address %p", addr);
  assert(is_aligned(total, os::vm_page_size()), "Unaligned length %zx", total);

  size_t aligned = align_up(used, os::vm_page_size());
  size_t unused = total - aligned;
  void *gap_addr = (char *) addr + aligned;

  SearchInIndex comparator;
  bool found;
  int at = _index.find_sorted<struct record>(&comparator, { .addr = (address) addr }, found);
  assert(found, "Cannot find region with address %p (%d records)", addr, _index.length());
  while (used > 0) {
    assert(at < _index.length(), "Overrunning index with 0x%zx used", used);
    const record &r = _index.at(at);
    // fprintf(stderr, "R %d %lx %lx %lx %x\n", at, r.addr, r.length, r.offset, r.flags);
    assert((void   *) r.addr == addr, "Unexpected address %p, expected %p", r.addr, addr);
    assert(r.flags & Flags::ACCESSIBLE, "Bad flags for %p: 0x%x", r.addr, r.flags);
    assert(r.length <= used, "Persisted memory region length does not match at %p: 0x%zx vs. 0x%zx", addr, used, r.length);
    if (r.flags & Flags::DATA) {
      assert(r.offset != BAD_OFFSET, "Invalid offset at %p", r.addr);
    } else {
      assert(r.offset == BAD_OFFSET, "Invalid offset at %p: 0x%zx", r.addr, r.offset);
    }
    used -= r.length;
    addr = (char *) addr + r.length;
    at++;
  }
  if (unused > 0) {
    const record &g = _index.at(at);
    assert(g.addr == gap_addr, "Invalid address for the gap region: %p vs. %p", g.addr, gap_addr);
    assert(g.length == unused, "Persisted gap length does not match at %p: 0x%zx vs. 0x%zx", gap_addr, unused, g.length);
    assert((g.flags & (Flags::DATA | Flags::ACCESSIBLE)) == Flags::ACCESSIBLE, "Bad flags for gap %p: 0x%x", gap_addr, g.flags);
    assert(g.offset == BAD_OFFSET, "Invalid offset at %p: 0x%zx", gap_addr, g.offset);
  }
}

void crac::MemoryPersister::assert_gap(void *addr, size_t length) {
  assert(is_aligned(addr, os::vm_page_size()), "Unaligned address %p", addr);
  assert(is_aligned(length, os::vm_page_size()), "Unaligned length 0x%zx", length);
  if (length > 0) {
    SearchInIndex comparator;
    bool found;
    int at = _index.find_sorted<struct record>(&comparator, { .addr = (address) addr }, found);
    assert(found, "Cannot find region with address %p (%d records)", addr, _index.length());
    const record &r = _index.at(at);
    assert(r.length == length, "Persisted memory region length does not match at %p: 0x%zx vs. 0x%zx", addr, length, r.length);
    assert((r.flags & (Flags::DATA | Flags::ACCESSIBLE)) == 0, "Bad flags for %p: 0x%x", addr, r.flags);
    assert(r.offset == BAD_OFFSET, "Invalid offset at %p: 0x%zx", addr, r.offset);
  }
}
#endif // ASSERT

void crac::MemoryPersister::finalize() {
  delete _writer;
  _writer = nullptr;

#ifdef ASSERT
  _index.sort([](struct record *a, struct record *b) {
    // simple cast to int doesn't work, let compiler figure it out with cmovs
    if (a->addr < b->addr) return -1;
    if (a->addr > b->addr) return 1;
    return 0;
  });
#endif // ASSERT
  // Note: here we could persist _index and dallocate it as well but since it's
  // usually tens or hundreds of 32 byte records, we won't save much.
}
