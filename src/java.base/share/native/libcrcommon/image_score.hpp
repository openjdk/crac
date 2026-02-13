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
#ifndef IMAGE_SCORE_HPP
#define IMAGE_SCORE_HPP

#include <cstring>
#include <cstdlib>
#include <cstdint>

#include "crcommon.hpp"
#include "crlib/crlib_image_score.h"
#include "linkedlist.hpp"

class ImageScore {
private:

  struct Score {
    const char* _name;
    double _value;

    Score(const char* name, double value): _name(name), _value(value) {}

    Score(Score &&o) {
      _name = o._name;
      _value = o._value;
      o._name = nullptr;
    }

    ~Score() {
      free((void*) _name);
    }
  };

  LinkedList<Score> _score;

public:
  bool set_score(const char* name, double value) {
    char* name_copy = strdup(name);
    if (name_copy == nullptr) {
      LOG("Cannot allocate copy of metric name");
      return false;
    }
    // Truncate metric name
    char *newline = strchr(name_copy, '\n');
    if (newline != nullptr) {
      *newline = '\0';
      LOG("warning: metric name '%s' contains a newline, truncating to '%s'", name, name_copy);
    }
    // We don't have expandable hashtable, so we'll sort out duplicates
    // in persist() when we have all the keys.
    return _score.add(Score(name_copy, value));
  }

  void reset_all() {
    _score.clear();
  }

  bool persist(const char* image_location) const;
};

#endif // IMAGE_SCORE_HPP
