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
#ifndef CRLIB_USER_DATA_H
#define CRLIB_USER_DATA_H

#include "crlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRLIB_EXTENSION_USER_DATA_NAME "user data"
#define CRLIB_EXTENSION_USER_DATA(api) \
  CRLIB_EXTENSION(api, crlib_user_data_t, CRLIB_EXTENSION_USER_DATA_NAME)

typedef struct crlib_user_data_storage crlib_user_data_storage_t;

// API for storing additional arbitrary data (user data) in checkpoint image.
struct crlib_user_data {
  crlib_extension_t header;

  // Records data to be stored under the specified name in a checkpoint image. Returns true on
  // success.
  // 'name' must be a valid non-empty C-string; if 'size' is positive 'data' must reference 'size'
  // bytes of data, if 'size' is 0 any data previously recorded under this name is cleared.
  bool (*set_user_data)(crlib_conf_t *, const char *name, const void *data, size_t size);

  // Prepares user data to be looked-up from a previously created and configured checkpoint image.
  // Returning a pointer to a managing structure or null on error.
  //
  // The other methods of this API can be used to interact with the returned structure.
  // 
  // The caller should destroy the structure after they are done using it. This should be done
  // before destroying the engine configuration that was used to create it.
  crlib_user_data_storage_t *(*load_user_data)(crlib_conf_t *);

  // Finds data with the specified name and writes a pointer to it to '*data_p' and the size of the
  // data to '*size_p'. Returns true on success.
  // 'user_data', 'data_p' and 'size_p' must not be null. 'name' must be a valid C-string.
  // Stored data should not be freed directly - destroy the managing structure instead.
  bool (*lookup_user_data)(crlib_user_data_storage_t *storage,
                           const char *name, const void **data_p, size_t *size_p);

  // Destroys the managing structure.
  void (*destroy_user_data)(crlib_user_data_storage_t *storage);
};
typedef const struct crlib_user_data crlib_user_data_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CRLIB_USER_DATA_H
