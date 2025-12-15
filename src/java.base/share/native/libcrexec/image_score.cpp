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
#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstring>

#include "crexec.hpp"
#include "hashtable.hpp"
#include "image_score.hpp"

bool ImageScore::persist(const char* image_location) const {
  char fname[PATH_MAX];
  if (snprintf(fname, sizeof(fname), "%s/score", image_location) >= (int) sizeof(fname) - 1) {
    fprintf(stderr, CREXEC "filename too long: %s/score\n", image_location);
    return false;
  }
  FILE* f = fopen(fname, "w");
  if (f == nullptr) {
    fprintf(stderr, CREXEC "cannot open %s for writing: %s\n", fname, strerror(errno));
    return false;
  }
  auto close_f = defer([&] {
    if (fclose(f)) {
     fprintf(stderr, CREXEC "cannot close %s/score: %s\n", image_location, strerror(errno));
    }
  });
  // Handle duplicates
  bool result = true;
  const char **keys = new(std::nothrow) const char*[_score.size()];
  if (keys == nullptr) {
    fprintf(stderr, CREXEC "cannot allocate array of metrics\n");
    return false;
  }
  auto delete_keys = defer([&] { delete[] keys; });
  size_t index = 0;
  _score.foreach([&](const Score& score){
    keys[index++] = score._name;
  });
  Hashtable<double> ht(keys, _score.size(), _score.size() * 3 / 2);
  if (!ht.is_initialized()) {
    fprintf(stderr, CREXEC "cannot create score hashtable (allocation failure)\n");
    return false;
  }
  _score.foreach([&](const Score& score) {
    ht.put(score._name, score._value);
  });
  // Make sure that we're using 'standard' format independent of locale
  // Ignore error, the reset with local 0 will be a noop
  locale_t c_locale = newlocale(LC_ALL_MASK, "C", 0);
  locale_t old_locale = uselocale(c_locale);
  ht.foreach([&](const char *key, double value){
    fprintf(f, "%s=%f\n", key, value);
  });
  uselocale(old_locale);
  freelocale(c_locale);
  return result;
}
