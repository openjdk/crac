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

#include "crlib/crlib_image_constraints.h"
#include "linkedlist.hpp"

class ImageConstraints {
private:
  enum TagType {
    LABEL,
    BITMAP,
  };

  struct Tag {
    TagType type;
    const char *name;
    const void *data;
    size_t data_length;

    ~Tag() {
        free((void *) name);
        free((void *) data);
    }

    Tag &operator=(Tag &&o) {
        type = o.type;
        name = o.name;
        data = o.data;
        data_length = o.data_length;
        o.name = nullptr;
        o.data = nullptr;
        return *this;
    }
  };

  struct Constraint {
    TagType type;
    const char *name;
    const void *data;
    size_t data_length;
    bitmap_comparison_t comparison;

    ~Constraint() {
        free((void *) name);
        free((void *) data);
    }

    Constraint &operator=(Constraint &&o) {
        type = o.type;
        name = o.name;
        data = o.data;
        data_length = o.data_length;
        comparison = o.comparison;
        o.name = nullptr;
        o.data = nullptr;
        return *this;
    }

    bool compare_bitmaps(const unsigned char *bitmap, size_t length) const;
  };

  LinkedList<Tag> _tags;
  LinkedList<Constraint> _constraints;

  const size_t _max_name_length = 256;
  const size_t _max_value_length = 256;

  bool check_tag(const char *name) {
    bool present = false;
    _tags.foreach([&](Tag &tag) {
      if (!strcmp(tag.name, name)) {
        present = true;
      }
    });
    return present;
  }

public:
  bool set_label(const char *name, const char *value) {
    if (check_tag(name)) {
      return false;
    }
    size_t value_length = strlen(value) + 1;
    if (strlen(name) >= _max_name_length || value_length >= _max_value_length) {
      return false;
    }
    _tags.add({
      .type = LABEL,
      .name = strdup(name),
      .data = strdup(value),
      .data_length = value_length,
    });
    return true;
  }

  bool set_bitmap(const char *name, const unsigned char *value, size_t length_bytes) {
    if (check_tag(name)) {
        return false;
    }
    if (strlen(name) >= _max_name_length || length_bytes >= _max_value_length) {
        return false;
    }
    void *copy = malloc(length_bytes);
    memcpy(copy, value, length_bytes);
    _tags.add({
      .type = BITMAP,
      .name = strdup(name),
      .data = (const unsigned char *) copy,
      .data_length = length_bytes,
    });
    return true;
  }

  void require_label(const char *name, const char *value) {
    _constraints.add({
      .type = LABEL,
      .name = strdup(name),
      .data = strdup(value),
      .data_length = strlen(value) + 1,
      .comparison = EQUALS,
    });
  }

  void require_bitmap(const char *name, const unsigned char *value, size_t length_bytes, bitmap_comparison_t comparison) {
    void *copy = malloc(length_bytes);
    memcpy(copy, value, length_bytes);
    _constraints.add({
      .type = BITMAP,
      .name = strdup(name),
      .data = copy,
      .data_length = length_bytes,
      .comparison = comparison,
    });
  }

  bool persist(const char *image_location) const;
  int validate(const char *image_location) const;
};

#endif // IMAGE_CONSTRAINTS_HPP
