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
#include <cassert>
#include <climits>
#include <cstdio>
#include <cerrno>
#include <utility>

#include "image_constraints.hpp"
#include "hashtable.hpp"

#define CREXEC "crexec: "
#define LABEL_PREFIX "label:"
#define BITMAP_PREFIX "bitmap:"

static FILE *open_tags(const char *image_location, const char *mode) {
  char fname[PATH_MAX];
  if (snprintf(fname, sizeof(fname), "%s/tags", image_location) >= (int) sizeof(fname) - 1) {
    fprintf(stderr, CREXEC "filename too long: %s/tags\n", image_location);
    return nullptr;
  }
  FILE *f = fopen(fname, mode);
  if (f == nullptr) {
    fprintf(stderr, CREXEC "cannot open(%s) %s: %s\n", fname, mode, strerror(errno));
    return nullptr;
  }
  return f;
}

bool ImageConstraints::set_label(const char *name, const char *value) {
  if (check_tag(name)) {
    fprintf(stderr, CREXEC "Label %s is already set\n", name);
    return false;
  }
  size_t value_length = strlen(value) + 1;
  if (strlen(name) >= _max_name_length || value_length >= _max_value_length) {
    fprintf(stderr, CREXEC "Label %s=%s is too long\n", name, value);
    return false;
  }
  char *name_copy = strdup(name);
  if (name_copy == nullptr) {
    fprintf(stderr, CREXEC "out of memory\n");
    return false;
  }
  const char *value_copy = strdup(value);
  if (value_copy == nullptr) {
    fprintf(stderr, CREXEC "out of memory\n");
    free(name_copy);
    return false;
  }
  _tags.add({
    .type = LABEL,
    .name = name_copy,
    .data = value_copy,
    .data_length = value_length,
  });
  return true;
}

bool ImageConstraints::set_bitmap(const char *name, const unsigned char *value, size_t length_bytes) {
  if (check_tag(name)) {
      fprintf(stderr, CREXEC "Bitmap %s is already set\n", name);
      return false;
  }
  if (strlen(name) >= _max_name_length || length_bytes >= _max_value_length) {
      fprintf(stderr, CREXEC "Bitmap %s=(%zu bytes) is too long\n", name, length_bytes);
      return false;
  }
  char *name_copy = strdup(name);
  if (name_copy == nullptr) {
    fprintf(stderr, CREXEC "out of memory\n");
    return false;
  }
  void *bitmap_copy = malloc(length_bytes);
  if (bitmap_copy == nullptr) {
    fprintf(stderr, CREXEC "out of memory\n");
    free(name_copy);
    return false;
  }
  memcpy(bitmap_copy, value, length_bytes);
  _tags.add({
    .type = BITMAP,
    .name = name_copy,
    .data = (const unsigned char *) bitmap_copy,
    .data_length = length_bytes,
  });
  return true;
}

bool ImageConstraints::persist(const char *image_location) const {
  FILE *f = open_tags(image_location, "w");
  if (f == nullptr) {
    return false;
  }
  _tags.foreach([&](const Tag &tag){
    if (tag.type == LABEL) {
      fprintf(f, LABEL_PREFIX "%s=%s\n", tag.name, static_cast<const char *>(tag.data));
    } else {
      fprintf(f, BITMAP_PREFIX "%s=", tag.name);
      const unsigned char *bytes = static_cast<const unsigned char *>(tag.data);
      for (const unsigned char *end = bytes + tag.data_length; bytes < end; bytes++) {
        fprintf(f, "%02x", *bytes);
      }
      fputc('\n', f);
    }
  });
  if (fclose(f)) {
    fprintf(stderr, CREXEC "cannot close %s/tags: %s\n", image_location, strerror(errno));
    return false;
  }
  return true;
}

static inline unsigned char from_hex(char c, bool *err) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  } else if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  } else {
    *err = true;
    return 0;
  }
}

static inline bool check_zeroes(const unsigned char *mem, size_t length) {
  for (const unsigned char *end = mem + length; mem < end; ++mem) {
    if (*mem) {
      return false;
    }
  }
  return true;
}

bool ImageConstraints::Constraint::compare_bitmaps(const unsigned char *bitmap, size_t length) const {
  size_t common_length = data_length < length ? data_length : length;
  if (comparison == EQUALS) {
    if (memcmp(data, bitmap, common_length)) {
      return false;
    }
    if (data_length > length && !check_zeroes(static_cast<const unsigned char *>(data) + common_length, data_length - common_length)) {
      return false;
    } else if (!check_zeroes(bitmap + common_length, length - common_length)) {
      return false;
    }
    return true;
  }
  const unsigned char *bm1 = bitmap, *bm2 = static_cast<const unsigned char *>(data);
  size_t l1 = length, l2 = data_length;
  if (comparison == SUPERSET) {
    bm1 = bm2;
    bm2 = bitmap;
    l1 = l2;
    l2 = length;
  }
  // Now test bm1 is subset of bm2
  for (size_t i = 0; i < common_length; ++i) {
    if ((bm1[i] & bm2[i]) != bm1[i]) {
      return false;
    }
  }
  if (l1 > l2) {
    return check_zeroes(bm1 + common_length, l1 - common_length);
  }
  return true;
}

static void print_bitmap(const char *name, const unsigned char *data, size_t length) {
  fprintf(stderr, CREXEC "\t%s", name);
  for (size_t i = 0; i < length; ++i) {
    fprintf(stderr, "%02x ", data[i]);
  }
  fputc('\n', stderr);
}

int ImageConstraints::validate(const char *image_location) const {
  FILE *f = open_tags(image_location, "r");
  if (f == nullptr) {
    return errno == ENOENT ? RESTORE_ERROR_NOT_FOUND : RESTORE_ERROR_NO_ACCESS;
  }
  char line[strlen(BITMAP_PREFIX) + _max_name_length + 1 + _max_value_length + 2];
  LinkedList<Tag> tags;
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    char *nl = strchr(eq + 1, '\n');
    if (eq == nullptr || nl == nullptr) {
      fprintf(stderr, CREXEC "Invalid format of tags file: %s\n", line);
      return RESTORE_ERROR_INVALID;
    }
    *eq = 0;
    *nl = 0;
    assert(eq < nl);
    if (!strncmp(line, LABEL_PREFIX, strlen(LABEL_PREFIX))) {
      tags.add({
        .type = LABEL,
        .name = strdup(line + strlen(LABEL_PREFIX)),
        .data = strdup(eq + 1),
        .data_length = (size_t) (nl - eq),
      });
    } else if (!strncmp(line, BITMAP_PREFIX, strlen(BITMAP_PREFIX))) {
      size_t length = (size_t)(nl - eq - 1)/2;
      if (2 * length != (size_t)(nl - eq - 1)) {
        fprintf(stderr, CREXEC "Invalid format of tags file (bad bitmap): %s\n", line);
        return RESTORE_ERROR_INVALID;
      }
      unsigned char *data = (unsigned char *) malloc(length);
      bool err = false;
      for (size_t i = 0; i < length; ++i) {
        data[i] = (from_hex(eq[1 + 2 * i], &err) << 4) + from_hex(eq[2 + 2 * i], &err);
      }
      if (err) {
        fprintf(stderr, CREXEC "Invalid format of tags file (bad character in bitmap): %s\n", line);
        return RESTORE_ERROR_INVALID;
      }
      tags.add({
        .type = BITMAP,
        .name = strdup(line + strlen(BITMAP_PREFIX)),
        .data = data,
        .data_length = length,
      });
    } else {
      fprintf(stderr, CREXEC "Invalid format of tags file (unknown type): %s\n", line);
      return RESTORE_ERROR_INVALID;
    }
  }
  const char **keys = new(std::nothrow) const char*[tags.size()];
  if (keys == nullptr) {
    fprintf(stderr, CREXEC "Insufficient memory\n");
    return RESTORE_ERROR_MEMORY;
  }
  int counter = 0;
  tags.foreach([&](const Tag &t) {
    keys[counter++] = t.name;
  });
  Hashtable<Tag> ht(keys, tags.size());
  delete[] keys;

  tags.foreach([&](Tag &t) {
    ht.put(t.name, std::move(t));
  });
  int result = 0;
  _constraints.foreach([&](const Constraint &c) {
    Tag *t = ht.get(c.name);
    if (t == nullptr) {
      fprintf(stderr, CREXEC "Tag %s was not found\n", c.name);
      result = RESTORE_ERROR_INVALID;
    } else if (t->type != c.type) {
      fprintf(stderr, CREXEC "Type mismatch for tag %s\n", c.name);
      result = RESTORE_ERROR_INVALID;
    } else if (c.type == LABEL && strcmp(static_cast<const char *>(c.data), static_cast<const char *>(t->data))) {
      fprintf(stderr, CREXEC "Label mismatch for tag %s: '%s' vs. '%s'\n", c.name,
        static_cast<const char *>(c.data), static_cast<const char *>(t->data));
      result = RESTORE_ERROR_INVALID;
    } else if (c.type == BITMAP && !c.compare_bitmaps(static_cast<const unsigned char *>(t->data), t->data_length)) {
      fprintf(stderr, CREXEC "Bitmap mismatch for tag %s:\n", c.name);
      print_bitmap("Constraint: ", static_cast<const unsigned char *>(c.data), c.data_length);
      print_bitmap("Image:      ", static_cast<const unsigned char *>(t->data), t->data_length);
      result = RESTORE_ERROR_INVALID;
    }
  });
  return result;
}
