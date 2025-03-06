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

#ifndef SHARE_RUNTIME_CRAC_ENGINE_HPP
#define SHARE_RUNTIME_CRAC_ENGINE_HPP

#include "crlib/crlib.h"
#include "crlib/crlib_description.h"
#include "crlib/crlib_restore_data.h"
#include "memory/allocation.hpp"
#include "nmt/memTag.hpp"

#include <cstddef>
#include <cstdint>

// CRaC engine library wrapper.
class CracEngine : public CHeapObj<mtInternal> {
public:
  explicit CracEngine(const char *image_location = nullptr);
  ~CracEngine();

  CracEngine(const CracEngine &) = delete;
  CracEngine &operator=(const CracEngine &) = delete;

  // Use this to check whether the constructor succeeded.
  bool is_initialized() const;

  // Operations supported by all engines

  int checkpoint() const;
  int restore() const;
  bool configure_image_location(const char *image_location) const;

  // Optionally-supported operations

  enum class ApiStatus : uint8_t { OK, ERR, UNSUPPORTED };

  ApiStatus prepare_restore_data_api();
  bool set_restore_data(const void *data, size_t size) const;
  size_t get_restore_data(void *buf, size_t size) const;

  ApiStatus prepare_description_api();
  const char *description() const;
  const char *configuration_doc() const;

private:
  void *_lib = nullptr;
  crlib_api_t *_api = nullptr;
  crlib_conf_t *_conf = nullptr;

  crlib_restore_data_t *_restore_data_api = nullptr;
  crlib_description_t *_description_api = nullptr;
};

#endif // SHARE_RUNTIME_CRAC_ENGINE_HPP
