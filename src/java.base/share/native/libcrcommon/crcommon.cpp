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
#include "crcommon.hpp"

#include "image_constraints.hpp"
#include "image_score.hpp"

static bool set_label(crlib_conf_t* conf, const char* name, const char* value) {
  return static_cast<ImageConstraints*>(conf->_image_constraints)->set_label(name, value);
}

static bool set_bitmap(crlib_conf_t* conf, const char* name, const unsigned char* value, size_t length_bytes) {
  return static_cast<ImageConstraints*>(conf->_image_constraints)->set_bitmap(name, value, length_bytes);
}

static bool require_label(crlib_conf_t* conf, const char* name, const char* value) {
  return static_cast<ImageConstraints*>(conf->_image_constraints)->require_label(name, value);
}

static bool require_bitmap(crlib_conf_t* conf, const char* name, const unsigned char *value, size_t length_bytes, crlib_bitmap_comparison_t comparison) {
  return static_cast<ImageConstraints*>(conf->_image_constraints)->require_bitmap(name, value, length_bytes, comparison);
}

static bool is_failed(crlib_conf_t* conf, const char* name) {
  return static_cast<ImageConstraints*>(conf->_image_constraints)->is_failed(name);
}

static bool set_score(crlib_conf_t* conf, const char* name, double value) {
  return static_cast<ImageScore*>(conf->_image_score)->set_score(name, value);
}

extern "C" {

const char* log_prefix = "<undefined>";

bool init_conf(struct crlib_conf* conf, const char* prefix) {
    log_prefix = prefix;
    if ((conf->_image_constraints = new(std::nothrow) ImageConstraints()) == nullptr) {
        return false;
    }
    if ((conf->_image_score = new(std::nothrow) ImageScore()) == nullptr) {
        delete static_cast<ImageConstraints*>(conf->_image_constraints);
        return false;
    }
    return true;
}

void destroy_conf(struct crlib_conf* conf) {
    delete static_cast<ImageConstraints*>(conf->_image_constraints);
    delete static_cast<ImageScore*>(conf->_image_score);
}

crlib_image_constraints_t image_constraints_extension = {
  {
    CRLIB_EXTENSION_IMAGE_CONSTRAINTS_NAME,
    sizeof(image_constraints_extension)
  },
  set_label,
  set_bitmap,
  require_label,
  require_bitmap,
  is_failed,
};

bool image_constraints_persist(const struct crlib_conf* conf, const char* image_location) {
  return static_cast<ImageConstraints*>(conf->_image_constraints)->persist(image_location);
}

bool image_constraints_validate(const struct crlib_conf* conf, const char* image_location) {
  return static_cast<ImageConstraints*>(conf->_image_constraints)->validate(image_location);
}

crlib_image_score_t image_score_extension {
  {
    CRLIB_EXTENSION_IMAGE_SCORE_NAME,
    sizeof(image_score_extension),
  },
  set_score,
};

bool image_score_persist(const struct crlib_conf* conf, const char* image_location) {
    return static_cast<ImageScore*>(conf->_image_score)->persist(image_location);
}

void image_score_reset(struct crlib_conf* conf) {
    static_cast<ImageScore*>(conf->_image_score)->reset_all();
}

const crlib_extension_t *find_extension(crlib_extension_t * const *extensions, const char *name, size_t size) {
  for (crlib_extension_t * const *ext = extensions; *ext != nullptr; ++ext) {
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
