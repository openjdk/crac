/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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
#ifndef IMAGE_CONSTRAINTS_HPP
#define IMAGE_CONSTRAINTS_HPP

#include <cstring>
#include <cstdlib>
#include <cstdint>

#include "crlib/crlib_image_constraints.h"
#include "linkedlist.hpp"

class ImageConstraints {
private:
  enum class TagType: std::uint8_t {
    LABEL,
    BITMAP,
  };

  struct Tag {
    TagType type;
    const char* name;
    const void* data;
    size_t data_size;

    Tag() = default;
    Tag(TagType t, const char* n, const void* d, size_t ds):
      type(t), name(n), data(d), data_size(ds) {}

    Tag(Tag &&o) {
      type = o.type;
      name = o.name;
      data = o.data;
      data_size = o.data_size;
      o.name = nullptr;
      o.data = nullptr;
    }

    ~Tag() {
      free((void*) name);
      free((void*) data);
    }

    Tag& operator=(Tag &&o) {
      type = o.type;
      name = o.name;
      data = o.data;
      data_size = o.data_size;
      o.name = nullptr;
      o.data = nullptr;
      return *this;
    }
  };

  struct Constraint {
    TagType type;
    bool failed;
    const char* name;
    const void* data;
    size_t data_size;
    const void* intersection; // data_size
    crlib_bitmap_comparison_t comparison;

    Constraint(TagType t, const char* n, const void* d, size_t ds, crlib_bitmap_comparison_t c):
      type(t), failed(false), name(n), data(d), data_size(ds), intersection(nullptr), comparison(c) {}

    Constraint(Constraint &&o) {
      type = o.type;
      name = o.name;
      data = o.data;
      data_size = o.data_size;
      intersection = o.intersection;
      comparison = o.comparison;
      o.name = nullptr;
      o.data = nullptr;
    }

    ~Constraint() {
      free((void*) name);
      free((void*) data);
      free((void *) intersection);
    }


    bool compare_bitmaps(const unsigned char* bitmap, size_t length) const;
  };

  LinkedList<Tag> _tags;
  LinkedList<Constraint> _constraints;

  static constexpr const size_t _MAX_NAME_SIZE = 256;
  static constexpr const size_t _MAX_VALUE_SIZE = 256;

  bool check_tag(const char* type, const char* name, size_t value_size);

public:
  bool set_label(const char* name, const char* value);
  bool set_bitmap(const char* name, const unsigned char* value, size_t length_bytes);

  bool require_label(const char* name, const char* value) {
    return _constraints.add(Constraint(TagType::LABEL, strdup(name), strdup(value), strlen(value) + 1, EQUALS));
  }

  bool require_bitmap(const char* name, const unsigned char* value, size_t length_bytes, crlib_bitmap_comparison_t comparison) {
    void* copy = malloc(length_bytes);
    memcpy(copy, value, length_bytes);
    return _constraints.add(Constraint(TagType::BITMAP, strdup(name), copy, length_bytes, comparison));
  }

  bool is_failed(const char* name, unsigned char *value_return) const {
    bool result = false;
    _constraints.foreach([&](Constraint &c) {
      if (!strcmp(c.name, name) && c.failed) {
        result = true;
        if (value_return && c.intersection) {
          memcpy(value_return, c.intersection, c.data_size);
        }
      }
    });
    return result;
  }

  bool persist(const char* image_location) const;
  bool validate(const char* image_location) const;
};

#endif // IMAGE_CONSTRAINTS_HPP
