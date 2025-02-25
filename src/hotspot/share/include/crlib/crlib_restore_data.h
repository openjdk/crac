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
#ifndef CRLIB_RESTORE_DATA_H
#define CRLIB_RESTORE_DATA_H

#include "crlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRLIB_EXTENSION_RESTORE_DATA_NAME "restore data"
#define CRLIB_EXTENSION_RESTORE_DATA(api) \
  CRLIB_EXTENSION(api, crlib_restore_data_t, CRLIB_EXTENSION_RESTORE_DATA_NAME)

// API for passing data from a restoring application to a restored application.
struct crlib_restore_data {
  crlib_extension_t header;

  // Called by the restoring application to pass data to the restored application.
  // 'data' must not be null, size must be greater than 0.
  // The engine may impose limits on the data size and return false if it is not accepted.
  bool (*set_restore_data)(crlib_conf_t *, const void *data, size_t size);

  // Called by the restored application to retrieve the data passed by the restoring process.
  // Copies up to 'size' > 0 bytes of the data into 'buf' != null.
  // Returns the size of the data the engine has, in bytes. Can be more, equal or less than 'size'.
  // Returned value of 0 represents an error.
  size_t (*get_restore_data)(crlib_conf_t *, void *buf, size_t size);
};
typedef const struct crlib_restore_data crlib_restore_data_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CRLIB_RESTORE_DATA_H
