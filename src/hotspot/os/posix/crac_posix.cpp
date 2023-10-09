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

// no precompiled headers
#include "jvm.h"
#include "runtime/crac.hpp"
#include "runtime/crac_structs.hpp"

#include <sys/mman.h>

int CracSHM::open(int mode) {
  int shmfd = shm_open(_path, mode, 0600);
  if (-1 == shmfd) {
    perror("shm_open");
  }
  return shmfd;
}

void CracSHM::unlink() {
  shm_unlink(_path);
}

#ifndef LINUX
void crac::vm_create_start() {
}

void VM_Crac::report_ok_to_jcmd_if_any() {
}

bool VM_Crac::check_fds() {
  return true;
}

bool crac::read_bootid(char *dest) {
  return true;
}

void crac::before_threads_persisted() {
}

void crac::after_threads_restored() {
}

#endif

bool crac::MemoryPersister::unmap(void *addr, size_t length) {
  while (::munmap(addr, length) != 0) {
    if (errno != EINTR) {
      perror("::munmap");
      return false;
    }
  }
  return true;
}

bool crac::MemoryPersister::map(void *addr, size_t length, os::ProtType protType) {
  unsigned int p = 0;
  switch (protType) {
  case os::ProtType::MEM_PROT_NONE: p = PROT_NONE; break;
  case os::ProtType::MEM_PROT_READ: p = PROT_READ; break;
  case os::ProtType::MEM_PROT_RW:   p = PROT_READ|PROT_WRITE; break;
  case os::ProtType::MEM_PROT_RWX:  p = PROT_READ|PROT_WRITE|PROT_EXEC; break;
  default:
    ShouldNotReachHere();
  }
  int mode = MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS;
#ifdef __APPLE__
  // Apple requires either R-X or RW- mappings unless MAP_JIT is present
  // but combination of MAP_FIXED and MAP_JIT is prohibited.
  assert(protType != os::ProtType::MEM_PROT_RWX, "Cannot create RWX mapping.");
#endif
  while (::mmap(addr, length, p, mode, -1 , 0) != addr) {
    if (errno != EINTR) {
      fprintf(stderr, "::mmap %p %zu RW: %s\n", addr, length, os::strerror(errno));
      return false;
    }
  }
  return true;
}

void crac::MmappingMemoryReader::read(size_t offset, void *addr, size_t size, bool executable) {
  assert(_fd >= 0, "File not open!");
  if (::mmap(addr, size, PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0),
      MAP_PRIVATE | MAP_FIXED, _fd , offset) != addr) {
    fatal("::mmap %p %zu RW(X): %s", addr, size, os::strerror(errno));
  }
}

