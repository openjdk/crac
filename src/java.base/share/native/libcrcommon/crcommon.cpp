/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
#include <cassert>

#include "crlib/crlib.h"
#include "crcommon.hpp"

#include "image_constraints.hpp"
#include "image_score.hpp"

static const char* log_prefix = "<undefined>";

struct crcommon {
  ImageConstraints image_constraints;
  ImageScore image_score;
};

#define COMMON(conf) reinterpret_cast<crlib_base_t*>(conf)->common()

static bool set_label(crlib_conf_t* conf, const char* name, const char* value) {
  return COMMON(conf)->image_constraints.set_label(name, value);
}

static bool set_bitmap(crlib_conf_t* conf, const char* name, const unsigned char* value, size_t length_bytes) {
  return COMMON(conf)->image_constraints.set_bitmap(name, value, length_bytes);
}

static bool require_label(crlib_conf_t* conf, const char* name, const char* value) {
  return COMMON(conf)->image_constraints.require_label(name, value);
}

static bool require_bitmap(crlib_conf_t* conf, const char* name, const unsigned char* value, size_t length_bytes, crlib_bitmap_comparison_t comparison) {
  return COMMON(conf)->image_constraints.require_bitmap(name, value, length_bytes, comparison);
}

static bool is_failed(crlib_conf_t* conf, const char* name) {
  return COMMON(conf)->image_constraints.is_failed(name);
}

static size_t get_failed_bitmap(crlib_conf_t* conf, const char* name, unsigned char* value_return, size_t value_size) {
  return COMMON(conf)->image_constraints.get_failed_bitmap(name, value_return, value_size);
}

static bool set_score(crlib_conf_t* conf, const char* name, double value) {
  return COMMON(conf)->image_score.set_score(name, value);
}

extern "C" {

JNIEXPORT crcommon_t* crcommon_create(const char* prefix) {
  log_prefix = prefix;
  crcommon_t* common = new(std::nothrow) crcommon();
  if (common == nullptr) {
    LOG("out of memory");
    return nullptr;
  }
  return common;
}

JNIEXPORT void crcommon_destroy(crcommon_t* common) {
  delete common;
}

JNIEXPORT const char* crcommon_log_prefix() {
  return log_prefix;
}

extern JNIEXPORT crlib_image_constraints_t image_constraints_extension = {
  {
    CRLIB_EXTENSION_IMAGE_CONSTRAINTS_NAME,
    sizeof(image_constraints_extension)
  },
  set_label,
  set_bitmap,
  require_label,
  require_bitmap,
  is_failed,
  get_failed_bitmap,
};

JNIEXPORT bool image_constraints_persist(const crcommon_t* conf, const char* image_location) {
  return conf->image_constraints.persist(image_location);
}

JNIEXPORT bool image_constraints_validate(const crcommon_t* conf, const char* image_location) {
  return conf->image_constraints.validate(image_location);
}

extern JNIEXPORT crlib_image_score_t image_score_extension = {
  {
    CRLIB_EXTENSION_IMAGE_SCORE_NAME,
    sizeof(image_score_extension),
  },
  set_score,
};

JNIEXPORT bool image_score_persist(const crcommon_t* conf, const char* image_location) {
  return conf->image_score.persist(image_location);
}

JNIEXPORT void image_score_reset(crcommon_t* conf) {
  conf->image_score.reset_all();
}

JNIEXPORT crlib_extension_t* find_extension(crlib_extension_t* const* extensions, const char* name, size_t size) {
  for (crlib_extension_t* const* ext = extensions; *ext != nullptr; ++ext) {
    if (strcmp(name, (*ext)->name) == 0) {
      if (size <= (*ext)->size) {
        return *ext;
      }
      return nullptr;
    }
  }
  return nullptr;
}

}
