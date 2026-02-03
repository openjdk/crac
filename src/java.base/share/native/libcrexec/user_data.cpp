/*
 * Copyright (c) 2023, 2026, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "crcommon.hpp"
#include "crlib/crlib_user_data.h"
#include "user_data.hpp"

struct user_data_chunk {
  struct user_data_chunk *next;
  size_t size;
  uint8_t *data;
};

bool UserData::set_user_data(const char *name, const void *data, size_t size) {
  if (!image_location()) {
    LOG("configure_image_location has not been called");
    return false;
  }
  char fname[PATH_MAX];
  if (snprintf(fname, sizeof(fname), "%s/%s", image_location(), name) >= (int) sizeof(fname) - 1) {
    LOG("filename too long: %s/%s", image_location(), name);
    return false;
  }
  FILE *f = fopen(fname, "w");
  if (f == nullptr) {
    LOG("cannot create %s: %s", fname, strerror(errno));
    return false;
  }
  while (size--) {
    uint8_t byte = *(const uint8_t *) data;
    data = (const void *) ((uintptr_t) data + 1);
    if (fprintf(f, "%02x", byte) != 2) {
      fclose(f);
      LOG("cannot write to %s: %s", fname, strerror(errno));
      return false;
    }
  }
  if (fputc('\n', f) != '\n') {
    fclose(f);
    LOG("cannot write to %s: %s", fname, strerror(errno));
    return false;
  }
  if (fclose(f)) {
    LOG("cannot close %s: %s", fname, strerror(errno));
    return false;
  }
  return true;
}

crlib_user_data_storage_t *UserData::load_user_data() {
  crlib_user_data_storage_t *user_data = static_cast<crlib_user_data_storage_t *>(malloc(sizeof(*user_data)));
  if (user_data == nullptr) {
    LOG("cannot allocate memory");
    return nullptr;
  }
  user_data->user_data = this;
  user_data->chunk = nullptr;
  return user_data;
}

bool UserData::lookup_user_data(crlib_user_data_storage_t *user_data, const char *name, const void **data_p, size_t *size_p) {
  if (!image_location()) {
    LOG("configure_image_location has not been called");
    return false;
  }
  char fname[PATH_MAX];
  if (snprintf(fname, sizeof(fname), "%s/%s", image_location(), name) >= (int) sizeof(fname) - 1) {
    LOG("filename is too long: %s/%s", image_location(), name);
    return false;
  }
  FILE *f = fopen(fname, "r");
  if (f == nullptr) {
    if (errno != ENOENT) {
      LOG("cannot open %s: %s", fname, strerror(errno));
    }
    return false;
  }
  uint8_t *data = nullptr;
  size_t data_used = 0;
  size_t data_allocated = 0;
  int nibble = -1;
  for (;;) {
    int gotc = fgetc(f);
    if (gotc == EOF) {
      fclose(f);
      free(data);
      LOG("unexpected EOF or error in %s after %zu parsed bytes", fname, data_used);
      return false;
    }
    if (gotc == '\n' && nibble == -1) {
      break;
    }
    if (gotc >= '0' && gotc <= '9') {
      gotc += -'0';
    } else if (gotc >= 'a' && gotc <= 'f') {
      gotc += -'a' + 0xa;
    } else {
      fclose(f);
      free(data);
      LOG("unexpected character 0x%02x in %s after %zu parsed bytes", gotc, fname, data_used);
      return false;
    }
    if (nibble == -1) {
      nibble = gotc;
      continue;
    }
    if (data_used == data_allocated) {
      data_allocated *= 2;
      if (!data_allocated) {
        data_allocated = 0x100;
      }
      uint8_t *data_new = static_cast<uint8_t *>(realloc(data, data_allocated));
      if (data_new == nullptr) {
        fclose(f);
        free(data);
        LOG("cannot allocate memory for %s after %zu parsed bytes", fname, data_used);
        return false;
      }
      data = data_new;
    }
    assert(data_used < data_allocated);
    data[data_used++] = (nibble << 4) | gotc;
    nibble = -1;
  }
  if (fgetc(f) != EOF || !feof(f) || ferror(f)) {
    fclose(f);
    free(data);
    LOG("EOF expected after newline in %s after %zu parsed bytes", fname, data_used);
    return false;
  }
  if (fclose(f)) {
    free(data);
    LOG("error closing %s after %zu parsed bytes", fname, data_used);
    return false;
  }
  *data_p = data;
  *size_p = data_used;
  struct user_data_chunk *chunk = static_cast<struct user_data_chunk *>(malloc(sizeof(*chunk)));
  if (chunk == nullptr) {
    free(data);
    LOG("cannot allocate memory");
    return false;
  }
  chunk->next = user_data->chunk;
  user_data->chunk = chunk;
  chunk->size = data_used;
  chunk->data = data;
  return true;
}

void UserData::destroy_user_data(crlib_user_data_storage_t *user_data) {
  while (user_data->chunk) {
    struct user_data_chunk *chunk = user_data->chunk;
    user_data->chunk = chunk->next;
    free(chunk->data);
    free(chunk);
  }
  free(user_data);
}
