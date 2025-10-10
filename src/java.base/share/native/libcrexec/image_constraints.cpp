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

#include "crexec.hpp"
#include "image_constraints.hpp"
#include "hashtable.hpp"

#define LABEL_PREFIX "label:"
#define BITMAP_PREFIX "bitmap:"

static FILE* open_tags(const char* image_location, const char* mode) {
  char fname[PATH_MAX];
  if (snprintf(fname, sizeof(fname), "%s/tags", image_location) >= (int) sizeof(fname) - 1) {
    fprintf(stderr, CREXEC "filename too long: %s/tags\n", image_location);
    return nullptr;
  }
  FILE* f = fopen(fname, mode);
  if (f == nullptr) {
    fprintf(stderr, CREXEC "cannot open %s in mode %s: %s\n", fname, mode, strerror(errno));
    return nullptr;
  }
  return f;
}

bool ImageConstraints::check_tag(const char* type, const char* name, size_t value_size) {
  bool present = false;
  _tags.foreach([&](Tag& tag) {
    if (!strcmp(tag.name, name)) {
      present = true;
    }
  });
  if (present) {
    fprintf(stderr, CREXEC "%s %s is already set\n", type, name);
  } else if (strpbrk(name, "=\n")) {
    fprintf(stderr, CREXEC "%s name must not contain '=' or newline\n", type);
  } else if (strlen(name) >= _MAX_NAME_SIZE) {
    fprintf(stderr, CREXEC "%s %s name is too long, at most %zu chars allowed\n",
      type, name, _MAX_NAME_SIZE - 1);
  } else if (value_size > _MAX_VALUE_SIZE) {
    fprintf(stderr, CREXEC "%s %s value is too long: %zu bytes > %zu allowed\n",
      type, name, value_size, _MAX_VALUE_SIZE);
  } else {
    return true;
  }
  return false;
}

bool ImageConstraints::set_label(const char* name, const char* value) {
  size_t value_size = strlen(value) + 1;
  if (!check_tag("Label", name, value_size)) {
    return false;
  } else if (strchr(value, '\n')) {
    fprintf(stderr, CREXEC "Label value must not contain a newline\n");
    return false;
  }
  char* name_copy = strdup(name);
  char* value_copy = strdup(value);
  if (name_copy == nullptr || value_copy == nullptr || !_tags.add(
      Tag(TagType::LABEL, name_copy, value_copy, value_size))) {
    fprintf(stderr, CREXEC "out of memory\n");
    free(name_copy);
    free(value_copy);
    return false;
  }
  return true;
}

bool ImageConstraints::set_bitmap(const char* name, const unsigned char* value, size_t value_size) {
  if (!check_tag("Bitmap", name, value_size)) {
      return false;
  }
  char* name_copy = strdup(name);
  void* bitmap_copy = malloc(value_size);
  if (bitmap_copy != nullptr) {
    memcpy(bitmap_copy, value, value_size);
  }
  if (name_copy == nullptr || bitmap_copy == nullptr || !_tags.add(
      Tag(TagType::BITMAP, name_copy, (const unsigned char*) bitmap_copy, value_size))) {
    fprintf(stderr, CREXEC "out of memory\n");
    free(name_copy);
    free(bitmap_copy);
    return false;
  }
  return true;
}

bool ImageConstraints::persist(const char* image_location) const {
  FILE* f = open_tags(image_location, "w");
  if (f == nullptr) {
    return false;
  }
  _tags.foreach([&](const Tag& tag){
    if (tag.type == TagType::LABEL) {
      fprintf(f, LABEL_PREFIX "%s=%s\n", tag.name, static_cast<const char*>(tag.data));
    } else {
      fprintf(f, BITMAP_PREFIX "%s=", tag.name);
      const unsigned char* bytes = static_cast<const unsigned char*>(tag.data);
      for (const unsigned char* end = bytes + tag.data_size; bytes < end; bytes++) {
        fprintf(f, "%02x",* bytes);
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

static inline unsigned char from_hex(char c, bool* err) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  } else if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  } else {
    *err = true;
    return 0;
  }
}

static inline bool check_zeroes(const unsigned char* mem, size_t length) {
  for (const unsigned char* end = mem + length; mem < end; ++mem) {
    if (*mem) {
      return false;
    }
  }
  return true;
}

bool ImageConstraints::Constraint::compare_bitmaps(const unsigned char* bitmap, size_t size) const {
  size_t common_size = data_size < size ? data_size : size;
  if (comparison == EQUALS) {
    if (memcmp(data, bitmap, common_size)) {
      return false;
    }
    if (data_size > size && !check_zeroes(static_cast<const unsigned char*>(data) + common_size, data_size - common_size)) {
      return false;
    } else if (!check_zeroes(bitmap + common_size, size - common_size)) {
      return false;
    }
    return true;
  }
  const unsigned char* bm1 = bitmap, * bm2 = static_cast<const unsigned char*>(data);
  size_t s1 = size, s2 = data_size;
  if (comparison == SUPERSET) {
    bm1 = bm2;
    bm2 = bitmap;
    s1 = s2;
    s2 = size;
  }
  // Now test bm1 is subset of bm2
  for (size_t i = 0; i < common_size; ++i) {
    if ((bm1[i] & bm2[i]) != bm1[i]) {
      return false;
    }
  }
  if (s1 > s2) {
    return check_zeroes(bm1 + common_size, s1 - common_size);
  }
  return true;
}

static void print_bitmap(const char* name, const unsigned char* data, size_t size) {
  fprintf(stderr, CREXEC "\t%s", name);
  for (size_t i = 0; i < size; ++i) {
    fprintf(stderr, "%02x ", data[i]);
  }
  fputc('\n', stderr);
}

bool ImageConstraints::validate(const char* image_location) const {
  FILE* f = open_tags(image_location, "r");
  if (f == nullptr) {
    return false;
  }
  char line[sizeof(BITMAP_PREFIX) + _MAX_NAME_SIZE + 1 + _MAX_VALUE_SIZE + 2];
  LinkedList<Tag> tags;
  while (fgets(line, (int) sizeof(line), f)) {
    char* eq = strchr((char *) line, '=');
    char* nl = strchr((char *) (eq + 1), '\n');
    if (eq == nullptr || nl == nullptr) {
      fprintf(stderr, CREXEC "Invalid format of tags file: %s\n", line);
      return false;
    }
    *eq = 0;
    *nl = 0;
    assert(eq < nl);
    if (!strncmp(line, LABEL_PREFIX, strlen(LABEL_PREFIX))) {
      char* name = strdup(line + strlen(LABEL_PREFIX));
      char* value = strdup(eq + 1);
      if (name == nullptr || value == nullptr || !tags.add({ TagType::LABEL, name, value, (size_t) (nl - eq) })) {
        fprintf(stderr, CREXEC "Cannot allocate memory for validation\n");
        free(name);
        free(value);
        return false;
      }
    } else if (!strncmp(line, BITMAP_PREFIX, strlen(BITMAP_PREFIX))) {
      size_t length = (size_t)(nl - eq - 1)/2;
      if (2 * length != (size_t)(nl - eq - 1)) {
        fprintf(stderr, CREXEC "Invalid format of tags file (bad bitmap): %s\n", line);
        return false;
      }
      unsigned char* data = (unsigned char*) malloc(length);
      if (data == nullptr) {
        fprintf(stderr, CREXEC "Cannot allocate memory for validation\n");
        return false;
      }
      bool err = false;
      for (size_t i = 0; i < length; ++i) {
        data[i] = (from_hex(eq[1 + 2 * i], &err) << 4) + from_hex(eq[2 + 2 * i], &err);
      }
      if (err) {
        fprintf(stderr, CREXEC "Invalid format of tags file (bad character in bitmap): %s\n", line);
        return false;
      }
      char* name = strdup(line + strlen(BITMAP_PREFIX));
      if (name == nullptr || !tags.add({ TagType::BITMAP, name, data, length })) {
        fprintf(stderr, CREXEC "Cannot allocate memory for validation\n");
        free(name);
        free(data);
        return false;
      }
    } else {
      fprintf(stderr, CREXEC "Invalid format of tags file (unknown type): %s\n", line);
      return false;
    }
  }
  const char** keys = new(std::nothrow) const char*[tags.size()];
  if (keys == nullptr) {
    fprintf(stderr, CREXEC "Insufficient memory\n");
    return false;
  }
  int counter = 0;
  tags.foreach([&](const Tag& t) {
    keys[counter++] = t.name;
  });
  Hashtable<Tag> ht(keys, tags.size());
  delete[] keys;

  bool result = true;
  tags.foreach([&](Tag& t) {
    result = ht.put(t.name, std::move(t)) && result;
  });
  _constraints.foreach([&](Constraint& c) {
    Tag* t = ht.get(c.name);
    c.failed = true;
    if (t == nullptr) {
      fprintf(stderr, CREXEC "Tag %s was not found\n", c.name);
    } else if (t->type != c.type) {
      fprintf(stderr, CREXEC "Type mismatch for tag %s\n", c.name);
    } else if (c.type == TagType::LABEL && strcmp(static_cast<const char*>(c.data), static_cast<const char*>(t->data))) {
      fprintf(stderr, CREXEC "Label mismatch for tag %s: '%s' vs. '%s'\n", c.name,
        static_cast<const char*>(c.data), static_cast<const char*>(t->data));
    } else if (c.type == TagType::BITMAP && !c.compare_bitmaps(static_cast<const unsigned char*>(t->data), t->data_size)) {
      fprintf(stderr, CREXEC "Bitmap mismatch for tag %s:\n", c.name);
      print_bitmap("Constraint: ", static_cast<const unsigned char*>(c.data), c.data_size);
      print_bitmap("Image:      ", static_cast<const unsigned char*>(t->data), t->data_size);
    } else {
      c.failed = false;
    }
    result = result && !c.failed;
  });
  return result;
}
