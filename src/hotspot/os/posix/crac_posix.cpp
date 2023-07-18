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

bool VM_Crac::memory_checkpoint() {
  return true;
}

void VM_Crac::memory_restore() {
}

bool crac::read_bootid(char *dest) {
  return true;
}
#endif