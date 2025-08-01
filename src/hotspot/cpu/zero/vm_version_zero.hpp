/*
 * Copyright (c) 2003, 2021, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2007 Red Hat, Inc.
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

#ifndef CPU_ZERO_VM_VERSION_ZERO_HPP
#define CPU_ZERO_VM_VERSION_ZERO_HPP

#include "runtime/abstract_vm_version.hpp"
#include "runtime/globals_extension.hpp"

class VM_Version : public Abstract_VM_Version {
 public:
  static void initialize();
  struct VM_Features {};
  static bool cpu_features_binary(VM_Features *data) { return false; }
  static bool cpu_features_binary_check(const VM_Features *data) { return data == nullptr; }
  static bool ignore_cpu_features() { return true; }

  constexpr static bool supports_stack_watermark_barrier() { return true; }

  static void initialize_cpu_information(void);
  static bool profile_all_receivers_at_type_check() { return false; }
};

#endif // CPU_ZERO_VM_VERSION_ZERO_HPP
